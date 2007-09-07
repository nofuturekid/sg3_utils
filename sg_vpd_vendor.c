/*
 * Copyright (c) 2006 Douglas Gilbert.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "sg_lib.h"
#include "sg_cmds_basic.h"

/* This is a companion file to sg_vpd.c . It contains logic to output and
   decode vendor specific VPD pages

   This program fetches Vital Product Data (VPD) pages from the given
   device and outputs it as directed. VPD pages are obtained via a
   SCSI INQUIRY command. Most of the data in this program is obtained
   from the SCSI SPC-4 document at http://www.t10.org .

   Acknowledgments:
      - Lars Marowsky-Bree <lmb at suse dot de> contributed Unit Path Report
        VPD page decoding for EMC CLARiiON devices [20041016]
      - Hannes Reinecke <hare at suse dot de> contributed RDAC vendor
        specific VPD pages [20060421]
*/


/* vendor VPD pages */
#define VPD_V_FIRM_SEA  0xc0
#define VPD_V_UPR_EMC  0xc0
#define VPD_V_DATC_SEA  0xc1
#define VPD_V_JUMP_SEA 0xc2
#define VPD_V_SVER_RDAC 0xc2
#define VPD_V_DEV_BEH_SEA 0xc3
#define VPD_V_VAC_RDAC 0xc9


#define DEF_ALLOC_LEN 252
#define MX_ALLOC_LEN (0xc000 + 0x80)
#define VPD_ATA_INFO_LEN  572

/* This structure is a duplicate of one of the same name in sg_vpd.c .
   Take care that both have the same fields (and types). */
struct svpd_values_name_t {
    int value;
    int subvalue;
    int pdt;         /* peripheral device type id, -1 is the default */
                     /* (all or not applicable) value */
    int ro_vendor;   /* read-only or vendor flag */
    const char * acron;
    const char * name;
};


static unsigned char rsp_buff[MX_ALLOC_LEN + 2];


/* Use subvalue to disambiguate between vendors */
static struct svpd_values_name_t vendor_vpd_pg[] = {
    {VPD_V_DATC_SEA, 0, -1, 1, "datc", "Date code (Seagate)"},
    {VPD_V_DEV_BEH_SEA, 0, -1, 1, "devb", "Device behavior (Seagate)"},
    {VPD_V_FIRM_SEA, 0, -1, 1, "firm", "Firmware numbers (Seagate)"},
    {VPD_V_JUMP_SEA, 0, -1, 1, "jump", "Jump setting (Seagate)"},
    {VPD_V_SVER_RDAC, 1, -1, 1, "sver", "Software version (RDAC)"},
    {VPD_V_UPR_EMC, 1, -1, 1, "upr", "Unit path report (EMC)"},
    {VPD_V_VAC_RDAC, 0, -1, 1, "vac", "Volume access control (RDAC)"},
    {0, 0, 0, 0, NULL, NULL},
};

static const struct svpd_values_name_t *
        svpd_get_v_detail(int page_num, int subvalue, int pdt)
{
    const struct svpd_values_name_t * vnp;
    int sv, ty;

    sv = (subvalue < 0) ? 1 : 0;
    ty = (pdt < 0) ? 1 : 0;
    for (vnp = vendor_vpd_pg; vnp->acron; ++vnp) {
        if ((page_num == vnp->value) &&
            (sv || (subvalue == vnp->subvalue)) &&
            (ty || (pdt == vnp->pdt)))
            return vnp;
    }
    if (! ty)
        return svpd_get_v_detail(page_num, subvalue, -1);
    if (! sv)
        return svpd_get_v_detail(page_num, -1, -1);
    return NULL;
}

const struct svpd_values_name_t *
                svpd_find_vendor_by_acron(const char * ap)
{
    const struct svpd_values_name_t * vnp;

    for (vnp = vendor_vpd_pg; vnp->acron; ++vnp) {
        if (0 == strcmp(vnp->acron, ap))
            return vnp;
    }
    return NULL;
}

void svpd_enumerate_vendor()
{
    const struct svpd_values_name_t * vnp;
    int seen;

    for (seen = 0, vnp = vendor_vpd_pg; vnp->acron; ++vnp) {
        if (vnp->name) {
            if (! seen) {
                printf("\nVendor specific VPD pages:\n");
                seen = 1;
            }
            printf("  %-10s 0x%02x,%d      %s\n", vnp->acron,
                   vnp->value, vnp->subvalue, vnp->name);
        }
    }
}

static void dStrRaw(const char* str, int len)
{
    int k;
    
    for (k = 0 ; k < len; ++k)
        printf("%c", str[k]);
}

static const char * lun_state_arr[] =
{
    "LUN not bound or LUN_Z report",
    "LUN bound, but not owned by this SP",
    "LUN bound and owned by this SP",
};

static const char * ip_mgmt_arr[] =
{
    "No IP access",
    "Reserved (undefined)",
    "via IPv4",
    "via IPv6",
};

static const char * sp_arr[] =
{
    "SP A",
    "SP B",
};

static const char * lun_op_arr[] =
{
    "Normal operations",
    "I/O Operations being rejected, SP reboot or NDU in progress",
};

static void decode_firm_vpd_c0_sea(unsigned char * buff, int len)
{
    if (len < 28) {
        fprintf(stderr, "Seagate firmware numbers VPD page length too "
                "short=%d\n", len);
        return;
    }
    if (28 == len) {
        printf("  SCSI firmware release number: %.8s\n", buff + 4);
        printf("  Servo ROM release number: %.8s\n", buff + 20);
    } else {
        printf("  SCSI firmware release number: %.8s\n", buff + 4);
        printf("  Servo ROM release number: %.8s\n", buff + 12);
        printf("  SAP block point numbers (major/minor): %.8s\n", buff + 20);
        if (len < 36)
            return;
        printf("  Servo firmware release date: %.4s\n", buff + 28);
        printf("  Servo ROM release date: %.4s\n", buff + 32);
        if (len < 44)
            return;
        printf("  SAP firmware release number: %.8s\n", buff + 36);
        if (len < 52)
            return;
        printf("  SAP firmware release date: %.4s\n", buff + 44);
        printf("  SAP firmware release year: %.4s\n", buff + 48);
        if (len < 60)
            return;
        printf("  SAP manufacturing key: %.4s\n", buff + 52);
        printf("  Servo firmware product family and product family "
               "member: %.4s\n", buff + 56);
    }
}

static void decode_upr_vpd_c0_emc(unsigned char * buff, int len)
{
    int k, ip_mgmt, failover_mode, vpp80, lun_z;

    if (len < 3) {
        fprintf(stderr, "EMC upr VPD page length too "
                "short=%d\n", len);
        return;
    }
    if (buff[9] != 0x00) {
        fprintf(stderr, "Unsupported page revision %d, decoding not "
                "possible.\n" , buff[9]);
        return;
    }
    printf("  LUN WWN: ");
    for (k = 0; k < 16; ++k)
        printf("%02hhx", buff[10 + k]);
    printf("\n");
    printf("  Array Serial Number: ");
    dStrRaw((const char *)&buff[50], buff[49]);
    printf("\n");

    printf("  LUN State: ");
    if (buff[4] > 0x02)
           printf("Unknown (%hhx)\n", buff[4]);
    else
           printf("%s\n", lun_state_arr[buff[4]]);

    printf("  This path connects to: ");
    if (buff[8] > 0x01)
           printf("Unknown SP (%hhx)", buff[8]);
    else
           printf("%s", sp_arr[buff[8]]);
    printf(", Port Number: %u\n", buff[7]);

    printf("  Default Owner: ");
    if (buff[5] > 0x01)
           printf("Unknown (%hhx)\n", buff[5]);
    else
           printf("%s\n", sp_arr[buff[5]]);

    printf("  NO_ATF: %s, Access Logix: %s\n",
                   buff[6] & 0x80 ? "set" : "not set",
                   buff[6] & 0x40 ? "supported" : "not supported");

    ip_mgmt = (buff[6] >> 4) & 0x3;

    printf("  SP IP Management Mode: %s\n", ip_mgmt_arr[ip_mgmt]);
    if (ip_mgmt == 2)
        printf("  SP IPv4 address: %u.%u.%u.%u\n",
               buff[44], buff[45], buff[46], buff[47]);
    else {
        printf("  SP IPv6 address: ");
        for (k = 0; k < 16; ++k)
            printf("%02hhx", buff[32 + k]);
        printf("\n");
    }

    failover_mode = buff[28] & 0x0f;
    vpp80 = buff[30] & 0x08;
    lun_z = buff[30] & 0x04;

    printf("  System Type: %hhx, ", buff[27]);
    switch (failover_mode) {
	case 4:
	    printf("Failover mode: 1 (Linux)\n");
	    break;
	case 6:
	    printf("Failover mode: 4 (ALUA)\n");
	    break;
	default:
	    printf("Failover mode: Unknown (%d)\n", failover_mode);
	    break;
    }

    printf("  Inquiry VPP 0x80 returns: %s, Arraycommpath: %s\n",
                   vpp80 ? "array serial#" : "LUN serial#",
                   lun_z ? "Set to 1" : "Unknown");

    printf("  Lun operations: %s\n",
               buff[48] > 1 ? "undefined" : lun_op_arr[buff[48]]);

    return;
}

static void decode_rdac_vpd_c2(unsigned char * buff, int len)
{
    if (len < 3) {
        fprintf(stderr, "Software Version VPD page length too "
                "short=%d\n", len);
        return;
    }
    if (buff[4] != 's' && buff[5] != 'w' && buff[6] != 'r') {
        fprintf(stderr, "Invalid page identifier %c%c%c%c, decoding "
                "not possible.\n" , buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    printf("  Software Version: %d.%d.%d\n", buff[8], buff[9], buff[10]);
    printf("  Software Date: %02x/%02x/%02x\n", buff[11], buff[12], buff[13]);
    printf("  Features:");
    if (buff[14] & 0x01)
        printf(" Dual Active,");
    if (buff[14] & 0x02)
        printf(" Series 3,");
    if (buff[14] & 0x04)
        printf(" Multiple Sub-enclosures,");
    if (buff[14] & 0x08)
        printf(" DCE/DRM,");
    if (buff[14] & 0x10)
        printf(" AVT,");
    printf("\n");
    printf("  Max. #of LUNS: %d\n", buff[15]);
    return;
}

static void decode_rdac_vpd_c9(unsigned char * buff, int len)
{
    if (len < 3) {
        fprintf(stderr, "Volume Access Control VPD page length too "
                "short=%d\n", len);
        return;
    }
    if (buff[4] != 'v' && buff[5] != 'a' && buff[6] != 'c') {
        fprintf(stderr, "Invalid page identifier %c%c%c%c, decoding "
                "not possible.\n" , buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    if (buff[7] != '1') {
        fprintf(stderr, "Invalid page version '%c' (should be 1)\n",
                buff[7]);
    }
    printf("  AVT:");
    if (buff[8] & 0x80) {
        printf(" Enabled");
        if (buff[8] & 0x40)
            printf(" (Allow reads on sector 0)");
        printf("\n");
    } else {
        printf(" Disabled\n");
    }
    printf("  Volume Access via: ");
    if (buff[8] & 0x01)
        printf("primary controller\n");
    else
        printf("alternate controller\n");

    printf("  Path priority: %d ", buff[9] & 0xf);
    switch(buff[9] & 0xf) {
        case 0x1:
            printf("(preferred path)\n");
            break;
        case 0x2:
            printf("(secondary path)\n");
            break;
        default:
            printf("(unknown)\n");
            break;
    }

    return;
}

/* Returns 0 if successful, see sg_ll_inquiry() plus SG_LIB_SYNTAX_ERROR for
   unsupported page */
int svpd_decode_vendor(int sg_fd, int num_vpd, int subvalue, int do_hex,
                       int do_raw, int do_long, int do_quiet, int verbose)
{
    int len, t, res;
    char name[64];
    const struct svpd_values_name_t * vnp;

    t = do_long;        /* suppress warning */
    vnp = svpd_get_v_detail(num_vpd, subvalue, -1);
    if (vnp && vnp->name)
        strcpy(name, vnp->name);
    else
        snprintf(name, sizeof(name) - 1, "Vendor VPD page=0x%x", num_vpd);
    switch(num_vpd) {
    case 0xc0:
        if ((! do_raw) && (! do_quiet))
            printf("%s VPD Page:\n", name);
        res = sg_ll_inquiry(sg_fd, 0, 1, VPD_V_UPR_EMC, rsp_buff,
                            DEF_ALLOC_LEN, 1, verbose);
        if (0 == res) {
            len = rsp_buff[3] + 4;
            if (VPD_V_UPR_EMC != rsp_buff[1]) {
                fprintf(stderr, "invalid VPD response; probably not "
                        "supported\n");
                return SG_LIB_CAT_MALFORMED;
            }
            if (len > MX_ALLOC_LEN) {
                fprintf(stderr, "response length too long: %d > %d\n", len,
                       MX_ALLOC_LEN);
                return SG_LIB_CAT_MALFORMED;
            } else if (len > DEF_ALLOC_LEN) {
                if (sg_ll_inquiry(sg_fd, 0, 1, VPD_V_UPR_EMC, rsp_buff, len, 1,
                           verbose))
                    return SG_LIB_CAT_OTHER;
            }
            if (do_raw)
                dStrRaw((const char *)rsp_buff, len);
            else if (do_hex)
                dStrHex((const char *)rsp_buff, len, 0);
            else if (0 == subvalue)
                decode_firm_vpd_c0_sea(rsp_buff, len);
            else if (1 == subvalue)
                decode_upr_vpd_c0_emc(rsp_buff, len);
            else
                dStrHex((const char *)rsp_buff, len, 0);
            return 0;
        }
        break;
    case VPD_V_SVER_RDAC:
        if ((! do_raw) && (! do_quiet))
            printf("%s VPD Page:\n", name);
        res = sg_ll_inquiry(sg_fd, 0, 1, VPD_V_SVER_RDAC, rsp_buff,
                            DEF_ALLOC_LEN, 1, verbose);
        if (0 == res) {
            len = rsp_buff[3] + 4;
            if (VPD_V_SVER_RDAC != rsp_buff[1]) {
                fprintf(stderr, "invalid VPD response; probably not "
                        "supported\n");
                return SG_LIB_CAT_MALFORMED;
            }
            if (len > MX_ALLOC_LEN) {
                fprintf(stderr, "response length too long: %d > %d\n", len,
                       MX_ALLOC_LEN);
                return SG_LIB_CAT_MALFORMED;
            } else if (len > DEF_ALLOC_LEN) {
                if (sg_ll_inquiry(sg_fd, 0, 1, VPD_V_SVER_RDAC, rsp_buff, len, 1,
                           verbose))
                    return SG_LIB_CAT_OTHER;
            }
            if (do_raw)
                dStrRaw((const char *)rsp_buff, len);
            else if (do_hex)
                dStrHex((const char *)rsp_buff, len, 0);
            else if (1 == subvalue)
                decode_rdac_vpd_c2(rsp_buff, len);
            else
                dStrHex((const char *)rsp_buff, len, 0);
            return 0;
        }
        break;
    case VPD_V_VAC_RDAC:
        if ((! do_raw) && (! do_quiet))
            printf("%s VPD Page:\n", name);
        res = sg_ll_inquiry(sg_fd, 0, 1, VPD_V_VAC_RDAC, rsp_buff,
                            DEF_ALLOC_LEN, 1, verbose);
        if (0 == res) {
            len = rsp_buff[3] + 4;
            if (VPD_V_VAC_RDAC != rsp_buff[1]) {
                fprintf(stderr, "invalid VPD response; probably not "
                        "supported\n");
                return SG_LIB_CAT_MALFORMED;
            }
            if (len > MX_ALLOC_LEN) {
                fprintf(stderr, "response length too long: %d > %d\n", len,
                       MX_ALLOC_LEN);
                return SG_LIB_CAT_MALFORMED;
            } else if (len > DEF_ALLOC_LEN) {
                if (sg_ll_inquiry(sg_fd, 0, 1, VPD_V_VAC_RDAC, rsp_buff, len, 1,
                           verbose))
                    return SG_LIB_CAT_OTHER;
            }
            if (do_raw)
                dStrRaw((const char *)rsp_buff, len);
            else if (do_hex)
                dStrHex((const char *)rsp_buff, len, 0);
            else if (0 == subvalue)
                decode_rdac_vpd_c9(rsp_buff, len);
            else
                dStrHex((const char *)rsp_buff, len, 0);
            return 0;
        }
        break;
    default:
        return SG_LIB_SYNTAX_ERROR;
    }
    return res;
}