#include <linux/kobject.h>
#include <linux/string.h>

#include "dev_info.h"
#include "external.h"
#include "ufshcd.h"

#define VENDOR_NAME_SIZE 16
#define PRODUCT_NAME_SIZE 32
#define RAM_SIZE 8
#define ROM_SIZE 8

typedef struct {
    char vendorName[VENDOR_NAME_SIZE];
    char productName[PRODUCT_NAME_SIZE];
    char ramSize[RAM_SIZE];
    char romSize[ROM_SIZE];
} FLASH_INFO_SETTINGS;

// TN Begin modify To add MICRON info by linzhi 20240104 OCALLA-1079
FLASH_INFO_SETTINGS flash_info_settings[] = {
    {
        "SAMSUNG", /* Vendor name */
        "KM8F9001JM-B813", /* Product name */
        "8G", /* Ram size */
        "256G", /* ROM size */
    },
    {
        "SAMSUNG", /* Vendor name */
        "KM8V9001JM-B813", /* Product name */
        "8G", /* Ram size */
        "128G", /* ROM size */
    },
    {
        "SAMSUNG", /* Vendor name */
        "KM2L9001CM-B518", /* Product name */
        "6G", /* Ram size */
        "128G", /* ROM size */
    },
    {
        "MICRON", /* Vendor name */
        "MT256GAXAU4U2281", /* Product name */
        "8G", /* Ram size */
        "256G", /* ROM size */
    },
    {
        "MICRON", /* Vendor name */
        "MT128GAXAU2U227Y", /* Product name */
        "8G", /* Ram size */
        "128G", /* ROM size */
    },
};
// TN End modify To add MICRON info by linzhi 20240104 OCALLA-1079

#define NUM_EMI_RECORD sizeof(flash_info_settings)/sizeof(FLASH_INFO_SETTINGS)

int num_of_emi_records = NUM_EMI_RECORD;

int get_flash_info(char *buf, void *arg0)
{
    int i = 0;
    char productName[PRODUCT_NAME_SIZE];
    struct ufs_hba *hba = (struct ufs_hba *)arg0;
    char *model = hba->dev_info.model;
    FLASH_INFO_SETTINGS *fis = flash_info_settings;

    memset(productName, 0, sizeof(productName));
    sprintf(productName, "%s", model);

    oemklog("%s:%d current productName is: %s\n", __func__, __LINE__, productName);
    for(i = 0; i < num_of_emi_records; i++) {
        if(strcmp(productName, fis[i].productName) == 0) {
            return sprintf(buf, "%s_%s+%s",
                fis[i].vendorName, fis[i].romSize, fis[i].ramSize);
        }
    }
    return 0;
}
EXPORT_SYMBOL(get_flash_info);
