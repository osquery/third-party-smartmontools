/*
 * nvmeprint.cpp
 *
 * Home page of code is: http://www.smartmontools.org
 *
 * Copyright (C) 2016-17 Christian Franke
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * You should have received a copy of the GNU General Public License
 * (for example COPYING); If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"
#include "nvmeprint.h"

const char * nvmeprint_cvsid = "$Id$"
  NVMEPRINT_H_CVSID;

#include "int64.h"
#include "utility.h"
#include "dev_interface.h"
#include "nvmecmds.h"
#include "atacmds.h" // dont_print_serial_number
#include "scsicmds.h" // dStrHex()
#include "smartctl.h"

using namespace smartmontools;

// Return true if 128 bit LE integer is != 0.
static bool le128_is_non_zero(const unsigned char (& val)[16])
{
  for (int i = 0; i < 16; i++) {
    if (val[i])
      return true;
  }
  return false;
}

// Format 128 bit integer for printing.
// Add value with SI prefixes if BYTES_PER_UNIT is specified.
static const char * le128_to_str(char (& str)[64], uint64_t hi, uint64_t lo, unsigned bytes_per_unit)
{
  if (!hi) {
    // Up to 64-bit, print exact value
    format_with_thousands_sep(str, sizeof(str)-16, lo);

    if (lo && bytes_per_unit && lo < 0xffffffffffffffffULL / bytes_per_unit) {
      int i = strlen(str);
      str[i++] = ' '; str[i++] = '[';
      format_capacity(str+i, (int)sizeof(str)-i-1, lo * bytes_per_unit);
      i = strlen(str);
      str[i++] = ']'; str[i] = 0;
    }
  }
  else {
    // More than 64-bit, print approximate value, prepend ~ flag
    snprintf(str, sizeof(str), "~%.0f",
             hi * (0xffffffffffffffffULL + 1.0) + lo);
  }

  return str;
}

// Format 128 bit LE integer for printing.
// Add value with SI prefixes if BYTES_PER_UNIT is specified.
static const char * le128_to_str(char (& str)[64], const unsigned char (& val)[16],
  unsigned bytes_per_unit = 0)
{
  uint64_t hi = val[15];
  for (int i = 15-1; i >= 8; i--) {
    hi <<= 8; hi += val[i];
  }
  uint64_t lo = val[7];
  for (int i =  7-1; i >= 0; i--) {
    lo <<= 8; lo += val[i];
  }
  return le128_to_str(str, hi, lo, bytes_per_unit);
}

// Format capacity specified as 64bit LBA count for printing.
static const char * lbacap_to_str(char (& str)[64], uint64_t lba_cnt, int lba_bits)
{
  return le128_to_str(str, (lba_cnt >> (64 - lba_bits)), (lba_cnt << lba_bits), 1);
}

// Format a Kelvin temperature value in Celsius.
static const char * kelvin_to_str(char (& str)[64], int k)
{
  if (!k) // unsupported?
    str[0] = '-', str[1] = 0;
  else
    snprintf(str, sizeof(str), "%d Celsius", k - 273);
  return str;
}

static inline unsigned le16_to_uint(const unsigned char (& val)[2])
{
  return ((val[1] << 8) | val[0]);
}

static void print_drive_info(const nvme_id_ctrl & id_ctrl, const nvme_id_ns & id_ns,
  unsigned nsid, bool show_all)
{
  char buf[64];
  pout("Model Number:                       %s\n", format_char_array(buf, id_ctrl.mn));
  if (!dont_print_serial_number)
    pout("Serial Number:                      %s\n", format_char_array(buf, id_ctrl.sn));
  pout("Firmware Version:                   %s\n", format_char_array(buf, id_ctrl.fr));

  // Vendor and Subsystem IDs are usually equal
  if (show_all || id_ctrl.vid != id_ctrl.ssvid) {
    pout("PCI Vendor ID:                      0x%04x\n", id_ctrl.vid);
    pout("PCI Vendor Subsystem ID:            0x%04x\n", id_ctrl.ssvid);
  }
  else {
    pout("PCI Vendor/Subsystem ID:            0x%04x\n", id_ctrl.vid);
  }

  pout("IEEE OUI Identifier:                0x%02x%02x%02x\n",
       id_ctrl.ieee[2], id_ctrl.ieee[1], id_ctrl.ieee[0]);

  // Capacity info is optional for devices without namespace management
  if (show_all || le128_is_non_zero(id_ctrl.tnvmcap) || le128_is_non_zero(id_ctrl.unvmcap)) {
    pout("Total NVM Capacity:                 %s\n", le128_to_str(buf, id_ctrl.tnvmcap, 1));
    pout("Unallocated NVM Capacity:           %s\n", le128_to_str(buf, id_ctrl.unvmcap, 1));
  }

  pout("Controller ID:                      %d\n", id_ctrl.cntlid);

  // Print namespace info if available
  pout("Number of Namespaces:               %u\n", id_ctrl.nn);

  if (nsid && id_ns.nsze) {
    const char * align = &("  "[nsid < 10 ? 0 : (nsid < 100 ? 1 : 2)]);
    int fmt_lba_bits = id_ns.lbaf[id_ns.flbas & 0xf].ds;

    // Size and Capacity are equal if thin provisioning is not supported
    if (show_all || id_ns.ncap != id_ns.nsze || (id_ns.nsfeat & 0x01)) {
      pout("Namespace %u Size:                 %s%s\n", nsid, align,
           lbacap_to_str(buf, id_ns.nsze, fmt_lba_bits));
      pout("Namespace %u Capacity:             %s%s\n", nsid, align,
           lbacap_to_str(buf, id_ns.ncap, fmt_lba_bits));
    }
    else {
      pout("Namespace %u Size/Capacity:        %s%s\n", nsid, align,
           lbacap_to_str(buf, id_ns.nsze, fmt_lba_bits));
    }
    // Utilization may be always equal to Capacity if thin provisioning is not supported
    if (show_all || id_ns.nuse != id_ns.ncap || (id_ns.nsfeat & 0x01))
      pout("Namespace %u Utilization:          %s%s\n", nsid, align,
           lbacap_to_str(buf, id_ns.nuse, fmt_lba_bits));

    pout("Namespace %u Formatted LBA Size:   %s%u\n", nsid, align, (1U << fmt_lba_bits));

    if (show_all || nonempty(id_ns.eui64, sizeof(id_ns.eui64)))
      pout("Namespace %u IEEE EUI-64:          %s%02x%02x%02x %02x%02x%02x%02x%02x\n",
           nsid, align, id_ns.eui64[0], id_ns.eui64[1], id_ns.eui64[2], id_ns.eui64[3],
           id_ns.eui64[4], id_ns.eui64[5], id_ns.eui64[6], id_ns.eui64[7]);
  }

  char td[DATEANDEPOCHLEN]; dateandtimezone(td);
  pout("Local Time is:                      %s\n", td);
}

// Format scaled power value.
static const char * format_power(char (& str)[16], unsigned power, unsigned scale)
{
  switch (scale & 0x3) {
    case 0: // not reported
      str[0] = '-'; str[1] = ' '; str[2] = 0; break;
    case 1: // 0.0001W
      snprintf(str, sizeof(str), "%u.%04uW", power / 10000, power % 10000); break;
    case 2: // 0.01W
      snprintf(str, sizeof(str), "%u.%02uW", power / 100, power % 100); break;
    default: // reserved
      str[0] = '?'; str[1] = 0; break;
  }
  return str;
}

static void print_drive_capabilities(const nvme_id_ctrl & id_ctrl, const nvme_id_ns & id_ns,
  unsigned nsid, bool show_all)
{
  pout("Firmware Updates (0x%02x):            %d Slot%s%s%s\n", id_ctrl.frmw,
       ((id_ctrl.frmw >> 1) & 0x7), (((id_ctrl.frmw >> 1) & 0x7) != 1 ? "s" : ""),
       ((id_ctrl.frmw & 0x01) ? ", Slot 1 R/O" : ""),
       ((id_ctrl.frmw & 0x10) ? ", no Reset required" : ""));

  if (show_all || id_ctrl.oacs)
    pout("Optional Admin Commands (0x%04x):  %s%s%s%s%s%s%s%s%s%s%s\n", id_ctrl.oacs,
         (!id_ctrl.oacs ? " -" : ""),
         ((id_ctrl.oacs & 0x0001) ? " Security" : ""),
         ((id_ctrl.oacs & 0x0002) ? " Format" : ""),
         ((id_ctrl.oacs & 0x0004) ? " Frmw_DL" : ""),
         ((id_ctrl.oacs & 0x0008) ? " NS_Mngmt" : ""),
         ((id_ctrl.oacs & 0x0010) ? " Self_Test" : ""), // NVMe 1.3 ...
         ((id_ctrl.oacs & 0x0020) ? " Directvs" : ""),
         ((id_ctrl.oacs & 0x0040) ? " MI_Snd/Rec" : ""),
         ((id_ctrl.oacs & 0x0080) ? " Vrt_Mngmt" : ""),
         ((id_ctrl.oacs & 0x0100) ? " Drbl_Bf_Cfg" : ""),
         ((id_ctrl.oacs & ~0x01ff) ? " *Other*" : ""));

  if (show_all || id_ctrl.oncs)
    pout("Optional NVM Commands (0x%04x):    %s%s%s%s%s%s%s%s%s\n", id_ctrl.oncs,
         (!id_ctrl.oncs ? " -" : ""),
         ((id_ctrl.oncs & 0x0001) ? " Comp" : ""),
         ((id_ctrl.oncs & 0x0002) ? " Wr_Unc" : ""),
         ((id_ctrl.oncs & 0x0004) ? " DS_Mngmt" : ""),
         ((id_ctrl.oncs & 0x0008) ? " Wr_Zero" : ""),
         ((id_ctrl.oncs & 0x0010) ? " Sav/Sel_Feat" : ""),
         ((id_ctrl.oncs & 0x0020) ? " Resv" : ""),
         ((id_ctrl.oncs & 0x0040) ? " Timestmp" : ""), // NVMe 1.3
         ((id_ctrl.oncs & ~0x007f) ? " *Other*" : ""));

  if (id_ctrl.mdts)
    pout("Maximum Data Transfer Size:         %u Pages\n", (1U << id_ctrl.mdts));
  else if (show_all)
    pout("Maximum Data Transfer Size:         -\n");

  // Temperature thresholds are optional
  char buf[64];
  if (show_all || id_ctrl.wctemp)
    pout("Warning  Comp. Temp. Threshold:     %s\n", kelvin_to_str(buf, id_ctrl.wctemp));
  if (show_all || id_ctrl.cctemp)
    pout("Critical Comp. Temp. Threshold:     %s\n", kelvin_to_str(buf, id_ctrl.cctemp));

  if (nsid && (show_all || id_ns.nsfeat)) {
    const char * align = &("  "[nsid < 10 ? 0 : (nsid < 100 ? 1 : 2)]);
    pout("Namespace %u Features (0x%02x):     %s%s%s%s%s%s%s\n", nsid, id_ns.nsfeat, align,
         (!id_ns.nsfeat ? " -" : ""),
         ((id_ns.nsfeat & 0x01) ? " Thin_Prov" : ""),
         ((id_ns.nsfeat & 0x02) ? " NA_Fields" : ""),
         ((id_ns.nsfeat & 0x04) ? " Dea/Unw_Error" : ""),
         ((id_ns.nsfeat & 0x08) ? " No_ID_Reuse" : ""), // NVMe 1.3
         ((id_ns.nsfeat & ~0x0f) ? " *Other*" : ""));
  }

  // Print Power States
  pout("\nSupported Power States\n");
  pout("St Op     Max   Active     Idle   RL RT WL WT  Ent_Lat  Ex_Lat\n");
  for (int i = 0; i <= id_ctrl.npss /* 1-based */ && i < 32; i++) {
    char p1[16], p2[16], p3[16];
    const nvme_id_power_state & ps = id_ctrl.psd[i];
    pout("%2d %c %9s %8s %8s %3d %2d %2d %2d %8u %7u\n", i,
         ((ps.flags & 0x02) ? '-' : '+'),
         format_power(p1, ps.max_power, ((ps.flags & 0x01) ? 1 : 2)),
         format_power(p2, ps.active_power, ps.active_work_scale),
         format_power(p3, ps.idle_power, ps.idle_scale),
         ps.read_lat & 0x1f, ps.read_tput & 0x1f,
         ps.write_lat & 0x1f, ps.write_tput & 0x1f,
         ps.entry_lat, ps.exit_lat);
  }

  // Print LBA sizes
  if (nsid && id_ns.lbaf[0].ds) {
    pout("\nSupported LBA Sizes (NSID 0x%x)\n", nsid);
    pout("Id Fmt  Data  Metadt  Rel_Perf\n");
    for (int i = 0; i <= id_ns.nlbaf /* 1-based */ && i < 16; i++) {
      const nvme_lbaf & lba = id_ns.lbaf[i];
      pout("%2d %c %7u %7d %9d\n", i, (i == id_ns.flbas ? '+' : '-'),
           (1U << lba.ds), lba.ms, lba.rp);
    }
  }
}

static void print_critical_warning(unsigned char w)
{
  pout("SMART overall-health self-assessment test result: %s\n",
       (!w ? "PASSED" : "FAILED!"));

  if (w) {
   if (w & 0x01)
     pout("- available spare has fallen below threshold\n");
   if (w & 0x02)
     pout("- temperature is above or below threshold\n");
   if (w & 0x04)
     pout("- NVM subsystem reliability has been degraded\n");
   if (w & 0x08)
     pout("- media has been placed in read only mode\n");
   if (w & 0x10)
     pout("- volatile memory backup device has failed\n");
   if (w & ~0x1f)
     pout("- unknown critical warning(s) (0x%02x)\n", w & ~0x1f);
  }

  pout("\n");
}

static void print_smart_log(const nvme_smart_log & smart_log, unsigned nsid,
  const nvme_id_ctrl & id_ctrl, bool show_all)
{
  char buf[64];
  pout("SMART/Health Information (NVMe Log 0x02, NSID 0x%x)\n", nsid);
  pout("Critical Warning:                   0x%02x\n", smart_log.critical_warning);
  pout("Temperature:                        %s\n",
       kelvin_to_str(buf, le16_to_uint(smart_log.temperature)));
  pout("Available Spare:                    %u%%\n", smart_log.avail_spare);
  pout("Available Spare Threshold:          %u%%\n", smart_log.spare_thresh);
  pout("Percentage Used:                    %u%%\n", smart_log.percent_used);
  pout("Data Units Read:                    %s\n", le128_to_str(buf, smart_log.data_units_read, 1000*512));
  pout("Data Units Written:                 %s\n", le128_to_str(buf, smart_log.data_units_written, 1000*512));
  pout("Host Read Commands:                 %s\n", le128_to_str(buf, smart_log.host_reads));
  pout("Host Write Commands:                %s\n", le128_to_str(buf, smart_log.host_writes));
  pout("Controller Busy Time:               %s\n", le128_to_str(buf, smart_log.ctrl_busy_time));
  pout("Power Cycles:                       %s\n", le128_to_str(buf, smart_log.power_cycles));
  pout("Power On Hours:                     %s\n", le128_to_str(buf, smart_log.power_on_hours));
  pout("Unsafe Shutdowns:                   %s\n", le128_to_str(buf, smart_log.unsafe_shutdowns));
  pout("Media and Data Integrity Errors:    %s\n", le128_to_str(buf, smart_log.media_errors));
  pout("Error Information Log Entries:      %s\n", le128_to_str(buf, smart_log.num_err_log_entries));

  // Temperature thresholds are optional
  if (show_all || id_ctrl.wctemp || smart_log.warning_temp_time)
    pout("Warning  Comp. Temperature Time:    %d\n", smart_log.warning_temp_time);
  if (show_all || id_ctrl.cctemp || smart_log.critical_comp_time)
    pout("Critical Comp. Temperature Time:    %d\n", smart_log.critical_comp_time);

  // Temperature sensors are optional
  for (int i = 0; i < 8; i++) {
    if (show_all || smart_log.temp_sensor[i])
      pout("Temperature Sensor %d:               %s\n", i + 1,
           kelvin_to_str(buf, smart_log.temp_sensor[i]));
  }
  if (show_all || smart_log.thm_temp1_trans_count)
    pout("Thermal Temp. 1 Transition Count:   %d\n", smart_log.thm_temp1_trans_count);
  if (show_all || smart_log.thm_temp2_trans_count)
    pout("Thermal Temp. 2 Transition Count:   %d\n", smart_log.thm_temp2_trans_count);
  if (show_all || smart_log.thm_temp1_total_time)
    pout("Thermal Temp. 1 Total Time:         %d\n", smart_log.thm_temp1_total_time);
  if (show_all || smart_log.thm_temp2_total_time)
    pout("Thermal Temp. 2 Total Time:         %d\n", smart_log.thm_temp2_total_time);
  pout("\n");
}

static void print_error_log(const nvme_error_log_page * error_log,
  unsigned num_entries, unsigned print_entries)
{
  pout("Error Information (NVMe Log 0x01, max %u entries)\n", num_entries);

  unsigned cnt = 0;
  for (unsigned i = 0; i < num_entries; i++) {
    const nvme_error_log_page & e = error_log[i];
    if (!e.error_count)
      continue; // unused or invalid entry
    if (++cnt > print_entries)
      continue;

    if (cnt == 1)
      pout("Num   ErrCount  SQId   CmdId  Status  PELoc          LBA  NSID    VS\n");

    char sq[16] = "-", cm[16] = "-", st[16] = "-", pe[16] = "-";
    char lb[32] = "-", ns[16] = "-", vs[8] = "-";
    if (e.sqid != 0xffff)
      snprintf(sq, sizeof(sq), "%d", e.sqid);
    if (e.cmdid != 0xffff)
      snprintf(cm, sizeof(cm), "0x%04x", e.cmdid);
    if (e.status_field != 0xffff)
      snprintf(st, sizeof(st), "0x%04x", e.status_field);
    if (e.parm_error_location != 0xffff)
      snprintf(pe, sizeof(pe), "0x%03x", e.parm_error_location);
    if (e.lba != 0xffffffffffffffffULL)
      snprintf(lb, sizeof(lb), "%" PRIu64, e.lba);
    if (e.nsid != 0xffffffffU)
      snprintf(ns, sizeof(ns), "%u", e.nsid);
    if (e.vs != 0x00)
      snprintf(vs, sizeof(vs), "0x%02x", e.vs);

    pout("%3u %10" PRIu64 " %5s %7s %7s %6s %12s %5s %5s\n",
         i, e.error_count, sq, cm, st, pe, lb, ns, vs);
  }

  if (!cnt)
    pout("No Errors Logged\n");
  else if (cnt > print_entries)
    pout("... (%u entries not shown)\n", cnt - print_entries);
  pout("\n");
}

int nvmePrintMain(nvme_device * device, const nvme_print_options & options)
{
  if (!(   options.drive_info || options.drive_capabilities
        || options.smart_check_status || options.smart_vendor_attrib
        || options.error_log_entries || options.log_page_size       )) {
    pout("NVMe device successfully opened\n\n"
         "Use 'smartctl -a' (or '-x') to print SMART (and more) information\n\n");
    return 0;
  }

  // Show unset optional values only if debugging is enabled
  bool show_all = (nvme_debugmode > 0);

  // Read Identify Controller always
  nvme_id_ctrl id_ctrl;
  if (!nvme_read_id_ctrl(device, id_ctrl)) {
    pout("Read NVMe Identify Controller failed: %s\n", device->get_errmsg());
    return FAILID;
  }

  // Print Identify Controller/Namespace info
  if (options.drive_info || options.drive_capabilities) {
    pout("=== START OF INFORMATION SECTION ===\n");
    nvme_id_ns id_ns; memset(&id_ns, 0, sizeof(id_ns));

    unsigned nsid = device->get_nsid();
    if (nsid == 0xffffffffU) {
      // Broadcast namespace
      if (id_ctrl.nn == 1) {
        // No namespace management, get size from single namespace
        nsid = 1;
        if (!nvme_read_id_ns(device, nsid, id_ns))
          nsid = 0;
      }
    }
    else {
        // Identify current namespace
        if (!nvme_read_id_ns(device, nsid, id_ns)) {
          pout("Read NVMe Identify Namespace 0x%x failed: %s\n", nsid, device->get_errmsg());
          return FAILID;
        }
    }

    if (options.drive_info)
      print_drive_info(id_ctrl, id_ns, nsid, show_all);
    if (options.drive_capabilities)
      print_drive_capabilities(id_ctrl, id_ns, nsid, show_all);
    pout("\n");
  }

  if (   options.smart_check_status || options.smart_vendor_attrib
      || options.error_log_entries)
    pout("=== START OF SMART DATA SECTION ===\n");

  // Print SMART Status and SMART/Health Information
  int retval = 0;
  if (options.smart_check_status || options.smart_vendor_attrib) {
    nvme_smart_log smart_log;
    if (!nvme_read_smart_log(device, smart_log)) {
      pout("Read NVMe SMART/Health Information failed: %s\n\n", device->get_errmsg());
      return FAILSMART;
    }

    if (options.smart_check_status) {
      print_critical_warning(smart_log.critical_warning);
      if (smart_log.critical_warning)
        retval |= FAILSTATUS;
    }

    if (options.smart_vendor_attrib) {
      print_smart_log(smart_log, device->get_nsid(), id_ctrl, show_all);
    }
  }

  // Print Error Information Log
  if (options.error_log_entries) {
    unsigned num_entries = id_ctrl.elpe + 1; // 0-based value
    raw_buffer error_log_buf(num_entries * sizeof(nvme_error_log_page));
    nvme_error_log_page * error_log =
      reinterpret_cast<nvme_error_log_page *>(error_log_buf.data());

    if (!nvme_read_error_log(device, error_log, num_entries)) {
      pout("Read Error Information Log failed: %s\n\n", device->get_errmsg());
      return retval | FAILSMART;
    }

    print_error_log(error_log, num_entries, options.error_log_entries);
  }

  // Dump log page
  if (options.log_page_size) {
    // Align size to dword boundary
    unsigned size = ((options.log_page_size + 4-1) / 4) * 4;
    raw_buffer log_buf(size);

    if (!nvme_read_log_page(device, options.log_page, log_buf.data(), size)) {
      pout("Read NVMe Log 0x%02x failed: %s\n\n", options.log_page, device->get_errmsg());
      return retval | FAILSMART;
    }

    pout("NVMe Log 0x%02x (0x%04x bytes)\n", options.log_page, size);
    dStrHex(log_buf.data(), size, 0);
    pout("\n");
  }

  return retval;
}
