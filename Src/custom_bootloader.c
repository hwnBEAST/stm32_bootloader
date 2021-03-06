/**
 * @file custom_bootloader.c
 *
 * @brief   Custom bootloader for STM32F4 Disc1 development board with
 *          STM32F407. Uses UART for communcation
 *
 * @note    Written according to BARR-C:2018 coding standard
 *          Exceptions:
 *              - 3.2 a, c  - Eclipse formater doesn't support
 *              - 6.3 b iv. - ERR_CHECK() macro has return keyword
 *              - 7.1 f     - Variables contain uppercase letters!
 *              - 7.1 h     - Uppercase letter can seperate words
 *              - 7.1 m     - Boolean begins with is, e.g. isExample
 */
#include "etc/cbl_common.h"
#include "custom_bootloader.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
/* Include what command types you want to support */
#if 1 == USE_CMDS_MEMORY
#include "commands/cbl_cmds_memory.h"
#endif
#if 1 == USE_CMDS_OPT_BYTES
#include "commands/cbl_cmds_opt_bytes.h"
#endif
#if 1 == USE_CMDS_ETC
#include "commands/cbl_cmds_etc.h"
#endif
#if 1 == USE_CMDS_UPDATE_NEW
#include "commands/cbl_cmds_update_new.h"
#endif
#if 1 == USE_CMDS_UPDATE_ACT
#include "commands/cbl_cmds_update_act.h"
#endif
#if 1 == USE_CMDS_TEMPLATE
#include "commands/cbl_cmds_template.h"
#endif

#define CMD_BUF_SZ 128 /*!< Size of a new command buffer */

#define TXT_CMD_VERSION "version"
#define TXT_CMD_HELP "help"
#define TXT_CMD_RESET "reset"

typedef enum
{
    STATE_OPER, /*!< Operational state */
    STATE_ERR, /*!< Error state */
    STATE_EXIT /*!< Deconstructor state */
} sys_states_t;

typedef enum
{
    CMD_UNDEF = 0,
    CMD_VERSION,
    CMD_HELP,
    CMD_CID,
    CMD_GET_RDP_LVL,
    CMD_JUMP_TO,
    CMD_FLASH_ERASE,
    CMD_EN_WRITE_PROT,
    CMD_DIS_WRITE_PROT,
    CMD_READ_SECT_PROT_STAT,
    CMD_MEM_READ,
    CMD_FLASH_WRITE,
    CMD_EXIT,
    CMD_TEMPLATE,
    CMD_RESET,
    CMD_UPDATE_NEW,
    CMD_UPDATE_ACT
} cmd_t;

static void shell_init (void);
static void go_to_user_app (void);
static cbl_err_code_t run_shell_system (void);
static cbl_err_code_t sys_state_operation (void);
static cbl_err_code_t wait_for_cmd (char * buf, size_t len);
static cbl_err_code_t enum_cmd (char * buf, size_t len, cmd_t * pCmdCode);
static cbl_err_code_t handle_cmd (cmd_t cmdCode, parser_t * phPrsr);
static cbl_err_code_t sys_state_error (cbl_err_code_t eCode);
static cbl_err_code_t cmd_version (parser_t * phPrsr);
static cbl_err_code_t cmd_help (parser_t * phPrsr);
static cbl_err_code_t cmd_reset (parser_t * phPrsr);

// \f - new page

/**
 * @brief Initializes HAL library
 */
void CBL_hal_init(void)
{
    hal_init();
}

/**
 * @brief Initializes all configured peripherals
 */
void CBL_periph_init(void)
{
    hal_periph_init();
}

/**
 * @brief   Gives control to the bootloader system. Bootloader system waits for
 *          a command from the host and blocks the thread until exit is
 *          requested or unrecoverable error happens.
 */
void CBL_run_system ()
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    INFO("Custom bootloader started\r\n");

    if (hal_blue_btn_state_get() == true)
    {
        INFO("Blue button pressed...\r\n");
    }
    else
    {
        INFO("Blue button not pressed...\r\n");
        eCode = run_shell_system();
    }

    ASSERT(CBL_ERR_OK == eCode, "ErrCode=%d:Restart the application.\r\n",
            eCode);
    go_to_user_app();
    ERROR("Switching to user application failed\r\n");
}

/**
 * @brief Handles only given command in 'cmd', doesn't give control to the
 * bootloader system
 *
 * @note Receives command directly from the programmer and not host
 * @note Function that returns textual data will print it out towards the host
 *       over defined peripheral
 * @note Expects a command without CRLF
 *
 * @param cmd[in] Command to process
 * @param len[in] length of cmd
 *
 * @return CBL_ERR_OK if no error, else error code
 */
cbl_err_code_t CBL_process_cmd (char * cmd, size_t len)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    cmd_t cmdCode = CMD_UNDEF;
    parser_t parser = { 0 };

    eCode = parser_run(cmd, len, &parser);
    ERR_CHECK(eCode);

    eCode = enum_cmd(parser.cmd, strlen(parser.cmd), &cmdCode);
    ERR_CHECK(eCode);

    eCode = handle_cmd(cmdCode, &parser);
    return eCode;
}

// \f - new page

/**
 * @brief   Notifies the user that bootloader started and initializes
 *          peripherals
 */
static void shell_init (void)
{
    char bufWelcome[] = CRLF
    "*********************************************" CRLF
    "Custom bootloader for STM32F4 Discovery board" CRLF
    "*********************************************" CRLF
    "*********************************************" CRLF
    "                     " CBL_VERSION CRLF
    "*********************************************" CRLF
    "               Master's thesis" CRLF
    "                  Dino Saric" CRLF
    "            University of Zagreb" CRLF
    "                     2020" CRLF
    "*********************************************" CRLF
    "          If confused type \"help\"          " CRLF
    "*********************************************" CRLF;

    hal_send_to_host(bufWelcome, strlen(bufWelcome));

    UNUSED( &hal_recv_from_host_stop);

    /* Bootloader started turn on red LED */
    hal_led_on(LED_POWER_ON);
}

// \f - new page
/**
 * @brief   Gives controler to the user application
 *          Steps:
 *          1)  Set the main stack pointer (MSP) to the one of the user
 *              application. User application MSP is contained in the first
 *              four bytes of flashed user application.
 *
 *          2)  Set the reset handler to the one of the user application.
 *              User application reset handler is right after the MSP, size of
 *              4 bytes.
 *
 *          3)  Jump to the user application reset handler. Giving control to
 *              the user application.
 *
 * @attention   DO NOT FORGET: In user application VECT_TAB_OFFSET set to the
 *          offset of user application from the start of the flash.
 *          e.g. If our application starts in the 2nd sector we would write
 *          #define VECT_TAB_OFFSET 0x8000.
 *          VECT_TAB_OFFSET is located in system_stm32f4xx.c
 *
 * @return  Procesor never returns from this application
 */
static void go_to_user_app (void)
{
    void (*pUserAppResetHandler) (void);
    uint32_t addressRstHndl;
    volatile uint32_t msp_value = *(volatile uint32_t *)CBL_ADDR_USERAPP;

    char userAppHello[] = "Jumping to user application :)\r\n";

    /* Send hello message to user and debug output */
    hal_send_to_host(userAppHello, strlen(userAppHello));
    INFO("%s", userAppHello);

    hal_deinit();

    addressRstHndl = *(volatile uint32_t *)(CBL_ADDR_USERAPP + 4u);

    pUserAppResetHandler = (void *)addressRstHndl;

    hal_disable_interrupts();

    /* WARNING: 32-bit assumed */
    DEBUG("MSP value: %#x\r\n", (unsigned int ) msp_value);
    DEBUG("Reset handler address: %#x\r\n", (unsigned int ) addressRstHndl);

    /* Reconfigure the vector table location */
    hal_vtor_set(CBL_ADDR_USERAPP);

    hal_stop_systick();

    /* Set the main stack pointer value */
    hal_msp_set(msp_value);

    /* Give control to user application */
    pUserAppResetHandler();

    /* Never reached */
}

// \f - new page
/**
 * @brief   Runs the shell for the bootloader until unrecoverable error happens
 *          or exit is requested
 *
 * @return  CBL_ERR_NO when no error, else returns an error code.
 */
static cbl_err_code_t run_shell_system (void)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    bool isExitNeeded = false;
    sys_states_t state = STATE_ERR;
    sys_states_t nextState = state;
    INFO("Starting bootloader\r\n");

    shell_init();
#ifdef CBL_CMDS_UPDATE_ACT_H
    /* Check if there is a update for user application */
    char update_at[] = TXT_CMD_UPDATE_ACT;
    eCode = CBL_process_cmd(update_at, strlen(update_at));
#endif /* CBL_CMDS_UPDATE_ACT_H */

    while (false == isExitNeeded)
    {
        switch (state)
        {
            case STATE_OPER:
            {
                eCode = sys_state_operation();

                /* Switch state if needed */
                if (eCode != CBL_ERR_OK)
                {
                    nextState = STATE_ERR;
                }
                else if (true == gIsExitReq)
                {
                    nextState = STATE_EXIT;
                }
                else
                {
                    /* Don't change state */
                }
            }
            break;

            case STATE_ERR:
            {
                eCode = sys_state_error(eCode);

                /* Switch state */
                if (eCode != CBL_ERR_OK)
                {
                    nextState = STATE_EXIT;
                }
                else
                {
                    nextState = STATE_OPER;
                }
            }
            break;

            case STATE_EXIT:
            {
                /* Deconstructor */
                char bye[] = "Exiting\r\n\r\n";

                INFO(bye);
                eCode = hal_send_to_host(bye, strlen(bye));
                ERR_CHECK(eCode);

                isExitNeeded = true;
            }
            break;

            default:
            {
                eCode = CBL_ERR_STATE;
            }
            break;

        }
        state = nextState;
    }
    /* Botloader done, turn off red LED */
    hal_led_off(LED_POWER_ON);

    return eCode;
}

// \f - new page
/**
 * @brief   Function that runs in normal operation, waits for new command from
 *          the host and processes it
 */
static cbl_err_code_t sys_state_operation (void)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    char cmd[CMD_BUF_SZ] = { 0 };

    hal_led_on(LED_READY);
    eCode = wait_for_cmd(cmd, CMD_BUF_SZ);
    ERR_CHECK(eCode);
    hal_led_off(LED_READY);

    hal_led_on(LED_BUSY);
    eCode = CBL_process_cmd(cmd, strlen(cmd));
    hal_led_off(LED_BUSY);
    return eCode;
}

// \f - new page
/**
 * @brief           Block thread until new command is received from host.
 *                  New command is considered received when CR LF is received
 *                  or buffer for command overflows
 *
 * @param buf[out]  Buffer for command
 *
 * @param len[in]   Length of buf
 */
static cbl_err_code_t wait_for_cmd (char * buf, size_t len)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    bool isLastCharCR = false;
    bool isOverflow = true;
    uint32_t iii = 0u;
    gRxCmdCntr = 0u;

    eCode = hal_send_to_host("\r\n> ", 4);
    ERR_CHECK(eCode);

    /* Read until CRLF or until full DMA */
    while (gRxCmdCntr < len)
    {
        /* Receive one char from host */
        eCode = hal_recv_from_host_start((uint8_t *)buf + iii, 1);
        ERR_CHECK(eCode);

        while (iii != (gRxCmdCntr - 1))
        {
            /* Wait for a new char */
        }

        if (true == isLastCharCR)
        {
            if ('\n' == buf[iii])
            {
                /* CRLF was received, command done */
                /* Replace '\r' with '\0' to make sure every char array ends
                 * with '\0' */
                buf[iii - 1] = '\0';
                isOverflow = false;
                break;
            }
        }

        /* Update isLastCharCR */
        isLastCharCR = '\r' == buf[iii] ? true : false;

        /* Prepare for next char */
        iii++;
    }
    /* If DMA fills the buffer and no CRLF is received throw an error */
    if (true == isOverflow)
    {
        eCode = CBL_ERR_READ_OF;
    }

    /* If no error buf has a new command to process */
    return eCode;
}

// \f - new page
/**
 * @brief               Enumerates the buf for command
 *
 * @param buf[in]       Buffer for command
 *
 * @param len[in]       Length of buffer
 *
 * @param pCmdCode[out] Pointer to command code enum
 *
 * @return              Error status
 */
static cbl_err_code_t enum_cmd (char * buf, size_t len, cmd_t * pCmdCode)
{
    cbl_err_code_t eCode = CBL_ERR_OK;

    if (0u == len)
    {
        eCode = CBL_ERR_CMD_SHORT;
    }
    else if (len == strlen(TXT_CMD_VERSION)
            && strncmp(buf, TXT_CMD_VERSION, strlen(TXT_CMD_VERSION)) == 0)
    {
        *pCmdCode = CMD_VERSION;
    }
    else if (len == strlen(TXT_CMD_HELP)
            && strncmp(buf, TXT_CMD_HELP, strlen(TXT_CMD_HELP)) == 0)
    {
        *pCmdCode = CMD_HELP;
    }
    else if (len == strlen(TXT_CMD_RESET)
            && strncmp(buf, TXT_CMD_RESET, strlen(TXT_CMD_RESET)) == 0)
    {
        *pCmdCode = CMD_RESET;
    }
#ifdef CBL_CMDS_ETC_H
    else if (len == strlen(TXT_CMD_CID)
            && strncmp(buf, TXT_CMD_CID, strlen(TXT_CMD_CID)) == 0)
    {
        *pCmdCode = CMD_CID;
    }
    else if (len == strlen(TXT_CMD_EXIT)
            && strncmp(buf, TXT_CMD_EXIT, strlen(TXT_CMD_EXIT)) == 0)
    {
        *pCmdCode = CMD_EXIT;
    }
#endif
#ifdef CBL_CMDS_OPT_BYTES_H
    else if (len == strlen(TXT_CMD_GET_RDP_LVL)
            && strncmp(buf, TXT_CMD_GET_RDP_LVL, strlen(TXT_CMD_GET_RDP_LVL))
                    == 0)
    {
        *pCmdCode = CMD_GET_RDP_LVL;
    }
    else if (len == strlen(TXT_CMD_EN_WRITE_PROT)
            && strncmp(buf, TXT_CMD_EN_WRITE_PROT,
                    strlen(TXT_CMD_EN_WRITE_PROT)) == 0)
    {
        *pCmdCode = CMD_EN_WRITE_PROT;
    }
    else if (len == strlen(TXT_CMD_DIS_WRITE_PROT)
            && strncmp(buf, TXT_CMD_DIS_WRITE_PROT,
                    strlen(TXT_CMD_DIS_WRITE_PROT)) == 0)
    {
        *pCmdCode = CMD_DIS_WRITE_PROT;
    }
    else if (len == strlen(TXT_CMD_READ_SECT_PROT_STAT)
            && strncmp(buf, TXT_CMD_READ_SECT_PROT_STAT,
                    strlen(TXT_CMD_READ_SECT_PROT_STAT)) == 0)
    {
        *pCmdCode = CMD_READ_SECT_PROT_STAT;
    }
#endif /* CBL_CMDS_OPT_BYTES_H */
#ifdef CBL_CMDS_MEMORY_H
    else if (len == strlen(TXT_CMD_JUMP_TO)
            && strncmp(buf, TXT_CMD_JUMP_TO, strlen(TXT_CMD_JUMP_TO)) == 0)
    {
        *pCmdCode = CMD_JUMP_TO;
    }
    else if (len == strlen(TXT_CMD_FLASH_ERASE)
            && strncmp(buf, TXT_CMD_FLASH_ERASE, strlen(TXT_CMD_FLASH_ERASE))
                    == 0)
    {
        *pCmdCode = CMD_FLASH_ERASE;
    }

    else if (len == strlen(TXT_CMD_MEM_READ)
            && strncmp(buf, TXT_CMD_MEM_READ, strlen(TXT_CMD_MEM_READ)) == 0)
    {
        *pCmdCode = CMD_MEM_READ;
    }
    else if (len == strlen(TXT_CMD_FLASH_WRITE)
            && strncmp(buf, TXT_CMD_FLASH_WRITE, strlen(TXT_CMD_FLASH_WRITE))
                    == 0)
    {
        *pCmdCode = CMD_FLASH_WRITE;
    }
#endif /* CBL_CMDS_MEMORY_H */
#ifdef CBL_CMDS_UPDATE_NEW_H
    else if (len == strlen(TXT_CMD_UPDATE_NEW)
            && strncmp(buf, TXT_CMD_UPDATE_NEW, strlen(TXT_CMD_UPDATE_NEW))
                    == 0)
    {
        *pCmdCode = CMD_UPDATE_NEW;
    }
#endif /* CBL_CMDS_UPDATE_NEW_H */
#ifdef CBL_CMDS_UPDATE_ACT_H
    else if (len == strlen(TXT_CMD_UPDATE_ACT)
            && strncmp(buf, TXT_CMD_UPDATE_ACT, strlen(TXT_CMD_UPDATE_ACT))
                    == 0)
    {
        *pCmdCode = CMD_UPDATE_ACT;
    }
#endif /* CBL_CMDS_UPDATE_ACT_H */
#ifdef CBL_CMDS_TEMPLATE_H
    /* Add a new enum value in cmd_t and check for it here */
    else if (len == strlen(TXT_CMD_TEMPLATE)
            && strncmp(buf, TXT_CMD_TEMPLATE, strlen(TXT_CMD_TEMPLATE)) == 0)
    {
        *pCmdCode = CMD_TEMPLATE;
    }
#endif /* CBL_CMDS_TEMPLATE_H */
    else
    {
        *pCmdCode = CMD_UNDEF;
    }

    if (CBL_ERR_OK == eCode && CMD_UNDEF == *pCmdCode)
    {
        eCode = CBL_ERR_CMD_UNDEF;
    }

    return eCode;
}

// \f - new page
/**
 * @brief               Handler for all defined commands
 *
 * @param cmdCode[in]   Enumerator for the commands
 *
 * @param phPrsr[in]    Handle of the parser containing parameters
 */
static cbl_err_code_t handle_cmd (cmd_t cmdCode, parser_t * phPrsr)
{
    cbl_err_code_t eCode = CBL_ERR_OK;

    switch (cmdCode)
    {
        case CMD_VERSION:
        {
            eCode = cmd_version(phPrsr);
        }
        break;

        case CMD_HELP:
        {
            eCode = cmd_help(phPrsr);
        }
        break;

        case CMD_RESET:
        {
            eCode = cmd_reset(phPrsr);
        }
        break;

#ifdef CBL_CMDS_OPT_BYTES_H
        case CMD_GET_RDP_LVL:
        {
            eCode = cmd_get_rdp_lvl(phPrsr);
        }
        break;

        case CMD_EN_WRITE_PROT:
        {
            eCode = cmd_change_write_prot(phPrsr, true);
        }
        break;

        case CMD_DIS_WRITE_PROT:
        {
            eCode = cmd_change_write_prot(phPrsr, false);
        }
        break;

        case CMD_READ_SECT_PROT_STAT:
        {
            eCode = cmd_get_write_prot(phPrsr);
        }
        break;
#endif /* CBL_CMDS_OPT_BYTES_H */
#ifdef CBL_CMDS_MEMORY_H
        case CMD_JUMP_TO:
        {
            eCode = cmd_jump_to(phPrsr);
        }
        break;

        case CMD_FLASH_ERASE:
        {
            eCode = cmd_flash_erase(phPrsr);
        }
        break;

        case CMD_MEM_READ:
        {
            eCode = cmd_mem_read(phPrsr);
        }
        break;

        case CMD_FLASH_WRITE:
        {
            eCode = cmd_flash_write(phPrsr);
        }
        break;
#endif /* CBL_CMDS_MEMORY_H */
#ifdef CBL_CMDS_UPDATE_NEW_H
        case CMD_UPDATE_NEW:
        {
            eCode = cmd_update_new(phPrsr);
        }
        break;
#endif /* CBL_CMDS_UPDATE_NEW_H */
#ifdef CBL_CMDS_UPDATE_ACT_H
        case CMD_UPDATE_ACT:
        {
            eCode = cmd_update_act(phPrsr);
        }
        break;
#endif /* CBL_CMDS_UPDATE_ACT_H */
#ifdef CBL_CMDS_ETC_H
        case CMD_CID:
        {
            eCode = cmd_cid(phPrsr);
        }
        break;

        case CMD_EXIT:
        {
            eCode = cmd_exit(phPrsr);
        }
        break;
#endif /* CBL_CMDS_ETC_H */
#ifdef CBL_CMDS_TEMPLATE_H
            /* Add a new case for the enumerator and call function handler */
            case CMD_TEMPLATE:
            {
                eCode = cmd_template(phPrsr);
            }
            break;
#endif /* CBL_CMDS_TEMPLATE_H */

        case CMD_UNDEF:
            /* No break */
        default:
        {
            eCode = CBL_ERR_CMDCD;
        }
        break;
    }

    if (eCode == CBL_ERR_OK)
    {
        /* Send success response */
        eCode = hal_send_to_host(TXT_SUCCESS, strlen(TXT_SUCCESS));
    }

    DEBUG("Responded\r\n");
    return eCode;
}

// \f - new page
/**
 * @brief           Handler for all errors
 *
 * @param eCode[in] Error code that happened
 */
static cbl_err_code_t sys_state_error (cbl_err_code_t eCode)
{
    DEBUG("Started\r\n");

    /* Turn off all LEDs except red */
    hal_led_off(LED_MEMORY);
    hal_led_off(LED_READY);
    hal_led_off(LED_BUSY);

    switch (eCode)
    {
        case CBL_ERR_OK:
            /* FALSE ALARM - no error */
        break;

        case CBL_ERR_READ_OF:
        {
            const char msg[] = "\r\nERROR: Command too long\r\n";
            WARNING("Overflow while reading happened\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_WRITE:
        {
            WARNING("Error occurred while writing\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_STATE:
        {
            WARNING("System entered unknown state, "
                    "returning to operational\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_HAL_TX:
        {
            WARNING("HAL transmit error happened\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_HAL_RX:
        {
            WARNING("HAL receive error happened\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_RX_ABORT:
        {
            WARNING("Error happened while aborting receive\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_CMD_SHORT:
        {
            INFO("Client sent an empty command\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_CMD_UNDEF:
        {
            const char msg[] = "\r\nERROR: Invalid command\r\n";
            INFO("Client sent an invalid command\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_NEED_PARAM:
        {
            const char msg[] = "\r\nERROR: Missing parameter(s)\r\n";
            INFO("Command is missing parameter(s)\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_JUMP_INV_ADDR:
        {
            const char msg[] = "\r\nERROR: Invalid address\r\n"
                    "Jumpable regions: FLASH, SRAM1, SRAM2, CCMRAM, "
                    "BKPSRAM, SYSMEM and EXTMEM (if connected)\r\n";

            INFO("Invalid address inputed for jumping\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_SECTOR:
        {
            const char msg[] =
                    "\r\nERROR: Internal error while erasing sectors\r\n";

            WARNING("Error while erasing sectors\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_SECT:
        {
            const char msg[] = "\r\nERROR: Wrong sector given\r\n";

            INFO("Wrong sector given\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_SECT_COUNT:
        {
            const char msg[] = "\r\nERROR: Wrong sector count given\r\n";

            INFO("Wrong sector count given\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_WRITE_INV_ADDR:
        {
            const char msg[] = "\r\nERROR: Invalid address range entered\r\n";

            INFO("Invalid address range entered for writing\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_SZ:
        {
            const char msg[] = "\r\nERROR: Invalid length\r\n";

            INFO("User entered length 0 or too big\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_HAL_WRITE:
        {
            const char msg[] = "\r\nERROR: Error while writing to flash."
                    " Retry last message.\r\n";

            INFO("Error while writing to flash on HAL level\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_ERASE_INV_TYPE:
        {
            const char msg[] = "\r\nERROR: Invalid erase type\r\n";

            INFO("User entered invalid erase type\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_HAL_ERASE:
        {
            const char msg[] = "\r\nERROR: HAL error while erasing sectors \r\n";

            INFO("HAL error while erasing sector\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_HAL_UNLOCK:
        {
            const char msg[] = "\r\nERROR: Unlocking flash failed\r\n";

            WARNING("Unlocking flash with HAL failed\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_PARAM:
        {
            ERROR("Wrong parameter sent to a function\r\n");
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_NOT_DIG:
        {
            const char msg[] =
                    "\r\nERROR: Number parameter contains letters\r\n";

            WARNING("User entered number parameter containing letters\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_1ST_NOT_ZERO:
        {
            const char msg[] =
                    "\r\nERROR: Number parameter must have '0' at the start "
                            " when 'x' is present\r\n";

            WARNING("User entered number parameter with 'x', "
                    "but not '0' on index 0\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_CKSUM_WRONG:
        {
            const char msg[] = "\r\nERROR: Data corrupted during transport"
                    " (Invalid checksum). Retry last message.\r\n";

            WARNING("Data corrupted during transport, invalid checksum\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_TEMP_NOT_VAL1:
        {
            const char msg[] = "\r\nERROR: Value for parameter invalid...\r\n";

            WARNING("User entered wrong param. value in template function\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_UNSUP_CKSUM:
        {
            const char msg[] = "\r\nERROR: Requested checksum not supported\r\n";

            WARNING("User requested checksum not supported\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_CRC_LEN:
        {
            const char msg[] = "\r\nERROR: Length for CRC32 must be "
                    "divisible by 4 \r\n";

            WARNING("User entered invalid length for CRC32\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_SHA256_LEN:
        {
            const char msg[] = "\r\nERROR: Invalid length for sha256\r\n";

            WARNING("User entered invalid length for sha256\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_NEW_APP_LEN:
        {
            const char msg[] = "\r\nERROR: New app is too long. Aborting\r\n";

            WARNING("New user application is too long"
                    "to for updating\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_NOT_IMPL:
        {
            const char msg[] =
                    "\r\nERROR: Requested action is not implemented\r\n";

            WARNING("Requested action is not implemented\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_APP_TYPE:
        {
            const char msg[] = "\r\nERROR: Invalid user application type\r\n";

            WARNING("Invalid user application type\r\n");
            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_NULL_PAR:
        {
            const char msg[] = "\r\nERROR: NULL sent as a parameter"
                    " of a function\r\n";

            WARNING("NULL sent as a parameter of a function\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_PAR_FORCE:
        {
            const char msg[] = "\r\nERROR: Invalid force parameter\r\n";

            WARNING("Invalid force parameter\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_SREC:
        {
            const char msg[] = "\r\nERROR: Invalid S-record file\r\n";

            WARNING("Invalid S-record file\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_SREC_FCN:
        {
            const char msg[] = "\r\nERROR: Invalid S-record function\r\n";

            WARNING("Invalid S-record function\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_HEX:
        {
            const char msg[] = "\r\nERROR: Invalid hex value character\r\n";

            WARNING("Invalid hex value character\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_SEGMEN:
        {
            const char msg[] = "\r\nERROR: Segmentation\r\n";

            WARNING("Tried accessing forbidden address\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_IHEX_FCN:
        {
            const char msg[] = "\r\nERROR: Unsupported Intel hex function\r\n";

            WARNING("Unsupported Intel hex function\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        case CBL_ERR_INV_IHEX:
        {
            const char msg[] = "\r\nERROR: Invalid contents of intel hex\r\n";

            WARNING("Invalid contents of intel hex\r\n");

            hal_send_to_host(msg, strlen(msg));
            eCode = CBL_ERR_OK;
        }
        break;

        default:
        {
            ERROR("Unhandled error happened\r\n");
        }
        break;

    }

    return eCode;
}

// \f - new page
/*********************************************************/
/* Fundamental function handles */
/*********************************************************/

/**
 * @brief Gets a version of the bootloader
 */
static cbl_err_code_t cmd_version (parser_t * phPrsr)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    char verbuf[12] = CBL_VERSION;

    DEBUG("Started\r\n");

    /* End with a new line */
    strlcat(verbuf, CRLF, 12);

    /* Send response */
    eCode = hal_send_to_host(verbuf, strlen(verbuf));

    return eCode;
}

// \f - new page
/**
 * @brief Returns string to the host of all commands
 */
static cbl_err_code_t cmd_help (parser_t * phPrsr)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    const char helpPrintout[] =
            "*************************************************************" CRLF
            "*************************************************************" CRLF
            "Custom STM32F4 bootloader shell by Dino Saric - " CBL_VERSION "***"
            "******" CRLF
            "*************************************************************" CRLF
            CRLF
            "*************************************************************" CRLF
            "Commands*****************************************************" CRLF
            "*************************************************************" CRLF
            CRLF
            "Optional parameters are surrounded with [] " CRLF CRLF
            "- " TXT_CMD_VERSION " | Gets the current version of the running "
            "bootloader" CRLF CRLF
            "- " TXT_CMD_HELP " | Makes life easier" CRLF CRLF
            "- " TXT_CMD_RESET " | Resets the microcontroller" CRLF CRLF
#ifdef CBL_CMDS_OPT_BYTES_H
            "- "
            TXT_CMD_GET_RDP_LVL
            " |  Read protection. Used to protect the"
            " software code stored in Flash memory."
            " Ref. man. p. 93" CRLF CRLF
            "- "
            TXT_CMD_EN_WRITE_PROT
            " | Enables write protection per sector,"
            " as selected with \""
            TXT_PAR_EN_WRITE_PROT_MASK
            "\"." CRLF
            "     "
            TXT_PAR_EN_WRITE_PROT_MASK
            " - Mask in hex form for sectors"
            " where LSB corresponds to sector 0." CRLF CRLF
            "- "
            TXT_CMD_DIS_WRITE_PROT
            " | Disables write protection per "
            "sector, as selected with \""
            TXT_PAR_EN_WRITE_PROT_MASK
            "\"." CRLF
            "     "
            TXT_PAR_EN_WRITE_PROT_MASK
            " - Mask in hex form for sectors"
            " where LSB corresponds to sector 0." CRLF CRLF
            "- "
            TXT_CMD_READ_SECT_PROT_STAT
            " | Returns bit array of sector "
            "write protection. LSB corresponds to sector 0. " CRLF CRLF
#endif /* CBL_CMDS_OPT_BYTES_H */
#ifdef CBL_CMDS_MEMORY_H
            "- " TXT_CMD_JUMP_TO
            " | Jumps to a requested address" CRLF
            "    " TXT_PAR_JUMP_TO_ADDR " - Address to jump to in hex format "
            "(e.g. 0x12345678), 0x can be omitted. " CRLF CRLF
            "- " TXT_CMD_FLASH_ERASE
            " | Erases flash memory" CRLF "    " TXT_PAR_FLASH_ERASE_TYPE
            " - Defines type of flash erase." CRLF
            "          \"" TXT_PAR_FLASH_ERASE_TYPE_MASS "\" - erases all "
            "sectors" CRLF
            "          \"" TXT_PAR_FLASH_ERASE_TYPE_SECT "\" - erases only "
            "selected sectors" CRLF
            "    " TXT_PAR_FLASH_ERASE_SECT " - First sector to erase. "
            "Bootloader is on sectors 0, 1 and 2. Not needed with mass erase."
            CRLF "    " TXT_PAR_FLASH_ERASE_COUNT
            " - Number of sectors to erase. Not needed with mass erase." CRLF
            CRLF "- " TXT_CMD_FLASH_WRITE " | Writes to flash byte by byte. "
            "Splits data into chunks" CRLF
            "     " TXT_PAR_FLASH_WRITE_START " - Starting address in hex "
            "format (e.g. 0x12345678), 0x can be omitted."CRLF
            "     " TXT_PAR_FLASH_WRITE_COUNT " - Number of bytes to write, "
            "without checksum. Chunk size: " TXT_FLASH_WRITE_SZ CRLF
            "     [" TXT_PAR_CKSUM "] - Checksum to use. If not"
            " present, no checksum is assumed" CRLF
            "             WARNING: Even if checksum is wrong data "
            "will be written into flash memory!" CRLF
            "                \"" TXT_CKSUM_SHA256 "\" - Best protection, "
            "slowest" CRLF
            "                \"" TXT_CKSUM_CRC "\" - Medium protection, fast,"
            " uses inbuilt CRC32 hardware." CRLF
            "                   Note: Data length must be divisible by 4! " CRLF
            "                   Settings:" CRLF
            "                            Polynomial: 0x4C11DB7 (Ethernet)" CRLF
            "                            Init value: 0xFFFFFFFF" CRLF
            "                                XORout: true" CRLF
            "                                 RefIn: true" CRLF
            "                                RefOut: true" CRLF
            "                \"" TXT_CKSUM_NO "\" - No protection, fastest"
            CRLF CRLF
            "- " TXT_CMD_MEM_READ
            " | Read bytes from memory" CRLF
            "     "
            TXT_PAR_FLASH_WRITE_START
            " - Starting address in hex "
            "format (e.g. 0x12345678), 0x can be omitted."CRLF
            "     "
            TXT_PAR_FLASH_WRITE_COUNT
            " - Number of bytes to read."
            CRLF CRLF
#endif /* CBL_CMDS_MEMORY_H */
#ifdef CBL_CMDS_UPDATE_ACT_H
            "- " TXT_CMD_UPDATE_ACT " | Updates active application from new "
            "application memory area" CRLF
            "     [" TXT_PAR_UP_ACT_FORCE "] - Forces update even if not "
            "needed" CRLF
            "                \"" TXT_PAR_UP_ACT_TRUE "\" - Force the "
            "update" CRLF
            "                \"" TXT_PAR_UP_ACT_FALSE "\" - Don't force the "
            "update" CRLF CRLF
#endif /* CBL_CMDS_UPDATE_ACT_H */
#ifdef CBL_CMDS_UPDATE_NEW_H
            "- " TXT_CMD_UPDATE_NEW " | Updates new application" CRLF
            "     " TXT_PAR_UP_NEW_COUNT " - Number of bytes to write, "
            "without checksum." CRLF
            "     " TXT_PAR_APP_TYPE " - Type of application coding" CRLF
            "                \"" TXT_PAR_APP_TYPE_BIN "\" - Binary format "
            "(.bin)" CRLF
            "                \"" TXT_PAR_APP_TYPE_HEX "\" - Intel hex "
            "format (.hex)" CRLF
            "                \"" TXT_PAR_APP_TYPE_SREC "\" - Motorola S-record"
            " format (.srec)" CRLF
            "     [" TXT_PAR_CKSUM "] - Checksum to use. If not"
            " present, no checksum is assumed" CRLF
            "             WARNING: Even if checksum is wrong data "
            "will be written into flash memory!" CRLF
            "                \"" TXT_CKSUM_SHA256 "\" - Best protection, "
            "slowest" CRLF
            "                \"" TXT_CKSUM_CRC "\" - Medium protection, fast,"
            " uses inbuilt CRC32 hardware." CRLF
            "                   Note: Data length must be divisible by 4! " CRLF
            "                   Settings:" CRLF
            "                            Polynomial: 0x4C11DB7 (Ethernet)" CRLF
            "                            Init value: 0xFFFFFFFF" CRLF
            "                                XORout: true" CRLF
            "                                 RefIn: true" CRLF
            "                                RefOut: true" CRLF
            "                \"" TXT_CKSUM_NO "\" - No protection, fastest"
            CRLF CRLF
#endif /* CBL_CMDS_UPDATE_NEW_H */
#ifdef CBL_CMDS_TEMPLATE_H
            /* Add a description of newly added command */
            TXT_CMD_TEMPLATE
            " | Explanation of function" CRLF
            "     "
            TXT_PAR_TEMPLATE_PARAM1
            " - Example param, valid value is: "
            TXT_PAR_TEMPLATE_VAL1
            CRLF CRLF
#endif /* CBL_CMDS_TEMPLATE_H */
#ifdef CBL_CMDS_ETC_H
            "- " TXT_CMD_CID " | Gets chip identification number" CRLF CRLF
            "- "TXT_CMD_EXIT " | Exits the bootloader and starts the user "
            "application" CRLF CRLF
#endif /* CBL_CMDS_ETC_H */
            "********************************************************" CRLF
            "Examples are contained in README.md" CRLF
            "********************************************************" CRLF;
    DEBUG("Started\r\n");
    /* Send response */
    eCode = hal_send_to_host(helpPrintout, strlen(helpPrintout));

    return eCode;
}

static cbl_err_code_t cmd_reset (parser_t * phPrsr)
{
    hal_send_to_host(TXT_SUCCESS, strlen(TXT_SUCCESS));

    hal_system_restart();

    /* Never returns */
    return CBL_ERR_OK;
}
/****END OF FILE****/
