/** @file cbl_cmds_update_new.c
 *
 * @brief Adds command for updating bytes for new application, writes to boot
 *        record
 */
#include "etc/cbl_checksum.h"
#include "commands/cbl_cmds_memory.h"
#include "commands/cbl_cmds_update_new.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static cbl_err_code_t update_new_get_params (parser_t * ph_prsr,
        uint32_t * p_len, cksum_t * p_cksum, app_type_t * p_app_type);

/**
 * @brief Updates new application bytes and writes to boot_record. On success
 *        restarts the processor
 *        Parameters from phPrsr:
 *          count - number of bytes to write
 *          cksum - checksum used
 *          type - application type (bin, hex...)
 *
 * @param phPrsr Pointer to handle of parser
 */
cbl_err_code_t cmd_update_new (parser_t * phPrsr)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    uint32_t len;
    cksum_t cksum;
    app_type_t app_type;
    boot_record_t * p_boot_record;

    eCode = update_new_get_params(phPrsr, &len, &cksum, &app_type);
    ERR_CHECK(eCode);

    eCode = hal_flash_erase_sector(BOOT_NEW_APP_START_SECTOR,
            BOOT_NEW_APP_MAX_SECTORS);
    ERR_CHECK(eCode);

    eCode = flash_write(BOOT_NEW_APP_START, len, cksum);
    ERR_CHECK(eCode);

    p_boot_record = boot_record_get();

    p_boot_record->new_app.app_type = app_type;
    p_boot_record->new_app.cksum_used = cksum;
    p_boot_record->new_app.len = len;

    p_boot_record->is_new_app_ready = true;

    eCode = boot_record_set(p_boot_record);
    ERR_CHECK(eCode);

    eCode = hal_send_to_host(TXT_SUCCESS, strlen(TXT_SUCCESS));
    ERR_CHECK(eCode);

    char restart_msg[] = "Restarting...\r\n";
    INFO("%s", restart_msg);
    eCode = hal_send_to_host(restart_msg, strlen(restart_msg));
    ERR_CHECK(eCode);

    hal_system_restart();

    /* NEVER REACHED */
    return eCode;
}

/**
 * @brief Gets the parameters for function update new application
 *
 * @param ph_prsr[in]      Pointer to parser with parameters
 * @param p_len[out]       Pointer to length of new application
 * @param p_cksum[out]     Pointer to checksum type
 * @param p_app_type[out]  Pointer to application type
 */
static cbl_err_code_t update_new_get_params (parser_t * ph_prsr,
        uint32_t * p_len, cksum_t * p_cksum, app_type_t * p_app_type)
{
    cbl_err_code_t eCode = CBL_ERR_OK;
    char *char_len = NULL;
    char *char_cksum = NULL;
    char *char_app_type = NULL;

    char_len = parser_get_val(ph_prsr, TXT_PAR_UP_NEW_COUNT,
            strlen(TXT_PAR_UP_NEW_COUNT));

    if (NULL == char_len)
    {
        return CBL_ERR_NEED_PARAM;
    }

    /* Fill len */
    eCode = str2ui32(char_len, strlen(char_len), p_len, 10u);
    ERR_CHECK(eCode);

    if (( *p_len) > BOOT_NEW_APP_MAX_LEN)
    {
        return CBL_ERR_NEW_APP_LEN;
    }

    char_cksum = parser_get_val(ph_prsr, TXT_PAR_CKSUM, strlen(TXT_PAR_CKSUM));

    eCode = enum_checksum(char_cksum, strlen(char_cksum), p_cksum);
    ERR_CHECK(eCode);

    char_app_type = parser_get_val(ph_prsr, TXT_PAR_APP_TYPE,
            strlen(TXT_PAR_APP_TYPE));

    if (NULL == char_app_type)
    {
        return CBL_ERR_NEED_PARAM;
    }

    eCode = enum_app_type(char_app_type, strlen(char_app_type), p_app_type);

    return eCode;
}

/*** end of file ***/
