/*
   ROCm GDB functions to manage debug information using the debug facilities
   library.

   Copyright (c) 2015-2016 ADVANCED MICRO DEVICES, INC.  All rights reserved.
   This file includes code originally published under

   Copyright (C) 1986-2014 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* GDB headers */
#include "defs.h"
#include "annotate.h"
#include "ui-out.h"
#include "format.h"
#include "gdb_assert.h"
#include "breakpoint.h"
#include "utils.h"

#include "rocm-dbginfo.h"
#include "rocm-segment-loader.h"
#include "rocm-tdep.h"
#include "rocm-utils.h"
#include "CommunicationControl.h"

/* Added for memcpy and file path processing*/
#include <libgen.h>
#include <string.h>

/* strtoull */
#include <stdlib.h>
#include <ctype.h>

/* Include HwDbgFacilities C interface*/
#include "FacilitiesInterface.h"

/* This buffer is static and managed by HwDbgFacilities */
static HwDbgInfo_debug gs_DbgInfo = NULL;
static char* gs_hsail_source = NULL;

static AgentBinaryNotification g_binary_notification = HSAIL_AGENT_BINARY_UNKNOWN;

/* A placeholder for when the filename is not present */
static const char default_file_name[] = "temp_source";

/* The active file used by the kernel, this will mostly be used in the cases where
 * the kerenl source is in memory and it is saved by the debugger
 * */
static char* active_kernel_src_file_path = NULL;

/*
 * These functions assume that we will only get one binary from the agent once
 * This function is called from the SIGHSAIL handler.
 * The SIGHSAIL handler is called when the inferior signals it's parent (gdb) to
 * tell it that we have a new binary
 */
void hsail_dbginfo_set_facilities_status(const AgentBinaryNotification notification)
{
  g_binary_notification = notification;
  if (notification == HSAIL_AGENT_BINARY_UNKNOWN)
    {
      hsail_free_hwdbginfo();
    }
}

static bool hsail_dbginfo_save_source_to_file(void)
{
  FILE* temp_file_handle = NULL;
  bool ret_code = false;
  const char* src_file_name =  hsail_dbginfo_get_active_file_name();

  /* if no source available, dont mess with anything to do with the file, bail*/
  if (gs_hsail_source == NULL)
    {
      return ret_code;
    }
  if (src_file_name == NULL)
    {
      return ret_code;
    }

  if (hsail_utils_check_file_exists(src_file_name))
    {
      if (!hsail_utils_set_file_permission(src_file_name, HSAIL_FILE_READ_WRITE))
        {
          printf_filtered("%s could not be made read-write\n",src_file_name);
        }
    }

  temp_file_handle = fopen(src_file_name,"wb");
  gdb_assert(temp_file_handle != NULL);

  fprintf(temp_file_handle, "%s",gs_hsail_source);

  rewind(temp_file_handle);
  fclose(temp_file_handle);

  if (!hsail_utils_set_file_permission(src_file_name, HSAIL_FILE_READ_ONLY))
    {
      printf_filtered("%s could not be made read only\n",src_file_name);;
    }

  rocm_printf_filtered("GPU kernel saved to %s\n",src_file_name);
  ret_code = true;
  return ret_code;

}

static bool hsail_dbginfo_init_source_buffer(HwDbgInfo_debug dbg_op)
{
  struct ui_out *uiout = NULL;
  bool ret_code = false;
  /* Get the kernel source */
  const char* temp_hsail_src = NULL;
  uint64_t hsail_source_len = 0;

  HwDbgInfo_err errOut = hwdbginfo_get_hsail_text(dbg_op, &temp_hsail_src, &hsail_source_len);
  if (errOut == HWDBGINFO_E_SUCCESS)
    {

      gdb_assert(temp_hsail_src != NULL);
      gdb_assert(hsail_source_len != 0);

      if (gs_hsail_source == NULL)
        {
          gs_hsail_source = xmalloc((hsail_source_len+1)*sizeof(char));
        }
      else
        {
          /* We need to resize the kernel source buffer for the new hwdbginfo object */
          free_current_contents(&gs_hsail_source);
          gs_hsail_source = xmalloc((hsail_source_len+1)*sizeof(char));
        }

      gdb_assert(gs_hsail_source != NULL);
      memset(gs_hsail_source, '\0', (hsail_source_len+1)*sizeof(char));
      memcpy(gs_hsail_source, temp_hsail_src, hsail_source_len*sizeof(char));

      ret_code = hsail_dbginfo_save_source_to_file();
      if (ret_code == true)
        {
          rocm_printf_filtered("Kernel saved to %s\n", active_kernel_src_file_path);
        }
      else
        {
          rocm_printf_filtered("Error saving kernel\n");
        }

    }
  else
    {
      size_t buffer_len = 1024;
      size_t file_name_len = 0;
      char* file_name = (char*)xmalloc(sizeof(char)*buffer_len);
      gdb_assert(file_name != NULL);

      memset(file_name, '\0', sizeof(char)*buffer_len);

      errOut = hwdbginfo_first_file_name(dbg_op, buffer_len, file_name, &file_name_len);
      if (errOut != HWDBGINFO_E_SUCCESS)
        {
          printf_filtered("Could not get the first filename ");
        }
      else
        {
          size_t source_len = 0;
          gdb_assert(errOut == HWDBGINFO_E_SUCCESS);

          hsail_utils_copy_string(&active_kernel_src_file_path, file_name);

          ret_code = hsail_utils_read_file_to_array(active_kernel_src_file_path,
                                                    &gs_hsail_source,
                                                    &source_len);
          if (ret_code == false)
            {
              rocm_printf_filtered("Could not read %s\n", active_kernel_src_file_path);
            }

          xfree(file_name);
        }
    }
  return ret_code;
}

const char* hsail_dbginfo_get_active_file_name(void)
{
  char* path_delim_posn = NULL;
  if (active_kernel_src_file_path == NULL)
    {
      hsail_utils_copy_string(&active_kernel_src_file_path, default_file_name);
    }

  path_delim_posn = strrchr(active_kernel_src_file_path, '/');
  if (path_delim_posn != NULL)
    {
      char* file_name = basename(active_kernel_src_file_path);
      return file_name;
    }
  else
    {
      return active_kernel_src_file_path;
    }

  return NULL;
}


/* Return line number and filename for an input PC
 * Note: The input pc has to be in the elf va form. The segment loader API should
 * resolve this before calling this function
 * */
bool hsail_dbginfo_get_pc_info(HwDbgInfo_addr pc,  HwDbgInfo_linenum* op_line_num, char** op_file_name)
{
  bool ret_code = false;
  HwDbgInfo_code_location loc = NULL;
  HwDbgInfo_debug dbg = hsail_init_hwdbginfo(NULL);
  HwDbgInfo_err err = HWDBGINFO_E_PARAMETER;

  HwDbgInfo_linenum line_num = 0;
  char* src_line = NULL;

  if (op_line_num == NULL || op_file_name == NULL)
    {
      return ret_code;
    }

  /* Go from PC to line by making a code_location object */
  if (dbg != NULL)
    {
      size_t out_filename_len =0;
      size_t in_filename_len =1024;
      char* out_filename = (char*)malloc(sizeof(char)*in_filename_len);
      gdb_assert(out_filename != NULL);
      memset(out_filename, '\0', sizeof(char)*in_filename_len );

      // for saxpy
      //pc += 4096;
      err = hwdbginfo_addr_to_line(dbg, pc, &loc);
      if (err != HWDBGINFO_E_SUCCESS)
        {
          printf("Debug facilities error %d", err);
        }
      err = hwdbginfo_code_location_details(loc,
                                            &line_num,
                                            in_filename_len, out_filename, &out_filename_len);
      if (err != HWDBGINFO_E_SUCCESS)
        {
          printf("Debug facilities in code location error %d", err);
        }

      if (out_filename != NULL)
        {
          if (strstr(out_filename, "hsa::self().elf") != NULL)
            {
              hsail_utils_copy_string(op_file_name,
                                      hsail_dbginfo_get_active_file_name());
            }
          else
            {
              *op_file_name = out_filename;
            }
        }
      *op_line_num = line_num;

      hwdbginfo_release_code_locations(&loc, 1);

      ret_code = true;
    }
  return ret_code;
}

/*
 * Initialize the hwdbginfo handle by calling the debug Facilities API
 * with the binary found in a shmem segment.
 *
 * The HwDbgInfo_debug object is cached. If there is already a binary loaded,
 * then this function will return a copy of the HwDbgInfo_debug object.
 * If you expect that a binary has already been loaded, then this function can
 * be called with NULL and the cached object will be returned
 */

/* This is a utility function to print the filename information */
int hsail_dbginfo_test_all_mapped_addrs(HwDbgInfo_debug dbg)
{
  HwDbgInfo_addr* addrs = NULL;
  HwDbgInfo_err err = 0;
  char* filename_buff = NULL;
  HwDbgInfo_code_location loc = NULL;
  size_t i = 0;
  size_t returned_filename_len = 0;
  size_t returned_addrCount = 0;

  /* Just a large buffer size */
  const size_t max_filename_len=10240;
  const size_t max_addrCount=10240;

  addrs = (HwDbgInfo_addr*)xmalloc(max_addrCount * sizeof(HwDbgInfo_addr));

  if (NULL == addrs)
    return -1;

  memset(addrs, 0, max_addrCount * sizeof(HwDbgInfo_addr));

  err = hwdbginfo_all_mapped_addrs(dbg,max_addrCount,addrs,&returned_addrCount);

  if (HWDBGINFO_E_SUCCESS != err)
    {
      printf("Error \n");
      return -1; /* unexpected error */
    }

  /* +1 to append null-terminator '\0' */
  filename_buff = (char*)xmalloc((max_filename_len+1)*sizeof(char));
  gdb_assert(NULL != filename_buff);
  if(NULL == filename_buff)
    return -1;

  memset(filename_buff, '\0', (max_filename_len+1)*sizeof(char));

  printf_filtered("Address count %lu\n",returned_addrCount);
  for(i=0; i < returned_addrCount; i++)
    {
      HwDbgInfo_linenum line_num = 0;
      err = hwdbginfo_addr_to_line(dbg,addrs[i],&loc);
      if (err != HWDBGINFO_E_SUCCESS)
        {
          printf_filtered("Couldn't resolve address 0x%llx",addrs[i]);
          break;
        }
      err = hwdbginfo_code_location_details(loc,
                                      &line_num,
                                      max_filename_len,
                                      filename_buff,
                                      &returned_filename_len);
      if (err != HWDBGINFO_E_SUCCESS)
        {
          printf_filtered("Couldn't resolve code location for address 0x%llx",addrs[i]);
          break;
        }
      printf_filtered("Address is 0x%llx  Line is %llu \n",addrs[i], line_num);
    }

  printf_filtered("Filename is %s \n",filename_buff);

  xfree(addrs);
  xfree(filename_buff);

  return 0;
}


char* hsail_dbginfo_get_source_buffer(void)
{
  struct ui_out *uiout = current_uiout;
  if (gs_hsail_source == NULL)
    {
      ui_out_text(uiout, "HSAIL source buffer not available\n");
    }

  return gs_hsail_source;
}

char* hsail_dbginfo_get_srcline_from_code_loc(const HwDbgInfo_debug dbg,
                                              const HwDbgInfo_code_location code_loc)
{
  HwDbgInfo_linenum line_num = 0;
  char* op_ptr = NULL;
  size_t buffer_len = 1024;
  size_t op_buffer_len = 1024;

  HwDbgInfo_err err = HWDBGINFO_E_SUCCESS;
  char* file_name = xmalloc(sizeof(char)*buffer_len);
  gdb_assert(file_name != NULL);
  memset(file_name, '\0', buffer_len);

  err = hwdbginfo_code_location_details(code_loc, &line_num, buffer_len ,file_name, &op_buffer_len);
  gdb_assert(err == HWDBGINFO_E_SUCCESS);

  if ((file_name == NULL) || (dbg == NULL))
    {
      return op_ptr;
    }

  /* if we are doing hsail level debugging, we need to look into memory */
  if( (strcmp(file_name, "temp_source") == 0) ||
      (strstr(file_name,"hsa::self().elf") != NULL) )
    {
      op_ptr = hsail_dbginfo_get_srcline_from_buffer(dbg, line_num);
    }
  else
    {
      op_ptr = hsail_utils_read_line_from_file(file_name, line_num);
    }

  xfree(file_name);
  return op_ptr;
}


bool hsail_dbginfo_search_linemapping(const char* ip_arg)
{

  bool ret_code = false;

  char* temp = NULL;
  char* ipStr = NULL;
  char* ipfileName = NULL;
  char* lineNoStr = NULL;
  HwDbgInfo_debug dbg = hsail_init_hwdbginfo(NULL);
  if (dbg == NULL)
    {
      return ret_code;
    }

  if (ip_arg == NULL)
    {
      return ret_code;
    }

  if (strlen(ip_arg) > 4)
    {
      if (ip_arg[0] == 'r' &&
          ip_arg[1] == 'o' &&
          ip_arg[2] == 'c' &&
          ip_arg[3] == 'm')
        {
          return ret_code;
        }
    }

  /* Copy input arg to a char array so strtok doesn't wreck input */
  hsail_utils_copy_string(&ipStr, ip_arg);

  temp = strtok(ipStr, ": ");

  if (temp == NULL)
    {
      return ret_code;
    }

  // Copy strtok op to a separate buffer
  hsail_utils_copy_string(&ipfileName, temp);

  lineNoStr = strtok(NULL, ": ");
  if (lineNoStr != NULL)
    {
      HwDbgInfo_linenum line_no = (HwDbgInfo_linenum) strtoull(lineNoStr, NULL, 10);

      // Create a code location
      HwDbgInfo_code_location loc = hwdbginfo_make_code_location(ipfileName, line_no);
      HwDbgInfo_code_location resolvedLoc = NULL;

      // Get the nearest code location
      HwDbgInfo_err err = hwdbginfo_nearest_mapped_line(dbg, loc, &resolvedLoc);

      if (err == HWDBGINFO_E_SUCCESS)
        {
          // Maybe compare the 2 ?
          HwDbgInfo_linenum resolvedLineNum;
          size_t resolvedfileNameLen = 1024;
          size_t resolvedfileNameLenOut = 0;

          char* fileName = (char*)malloc(resolvedfileNameLen*sizeof(char));
          gdb_assert(fileName != NULL);
          memset(fileName, '\0', resolvedfileNameLen);

          err = hwdbginfo_code_location_details(resolvedLoc, &resolvedLineNum,
                                                resolvedfileNameLen, fileName, &resolvedfileNameLenOut);
          if  (abs(resolvedLineNum - line_no) < 2)
            {
              printf("Create a GPU Breakpoint ");
              ret_code =true;
            }
          else
            {
              printf("Resolved %lld ", resolvedLineNum);
              printf("Req is %lld \n", line_no);
            }
          xfree(fileName);
        }
      else
        {
          printf("Not a GPU breakpoint %d",err);
        }
    }

  xfree(temp);
  xfree(ipfileName);

  return ret_code;
}


/* This function takes in the debuginfo handle so that it can
 * get the source buffer for gdb.
 */
char* hsail_dbginfo_get_srcline_from_buffer(const HwDbgInfo_debug dbg,
                                            const HwDbgInfo_linenum line_num)
{

  int temp_file_desc = -1;
  char temp_file_name[] = "/tmp/rocm_dbg_XXXXXX";

  FILE* temp_file_handle = NULL;
  char* op_line = NULL;
  int status = 0;

  gdb_assert(dbg != NULL);

  /* Get a temp file name, mkstemp will populate the XXXXXX in temp_file_name
   *  we dont use the desc, we will open the file using fopen*/
  temp_file_desc = mkstemp(temp_file_name);
  gdb_assert(temp_file_desc  != -1);

  temp_file_handle = fopen(temp_file_name,"wb");
  gdb_assert(temp_file_handle != NULL);

  fprintf(temp_file_handle, "%s",gs_hsail_source);

  fclose(temp_file_handle);
  op_line = hsail_utils_read_line_from_file(temp_file_name, line_num);

  /* Delete tmp file */
  status = remove(temp_file_name);
  gdb_assert(status == 0);

  /*op_line will be free'd by the breakpoint request that for the source line */
  return op_line;
}


AgentBinaryNotification hsail_is_debug_facilities_loaded(void)
{
  return g_binary_notification;
}

HwDbgInfo_debug hsail_init_hwdbginfo(HsailNotificationPayload* payload)
{
  struct ui_out *uiout = NULL;
  /* pointer to shared memory*/
  void* pShm = NULL;

  /* HwDbgFacilities objects */
  HwDbgInfo_err errout_twolevel= HWDBGINFO_E_UNEXPECTED;
  HwDbgInfo_err errout_onelevel= HWDBGINFO_E_UNEXPECTED;

  HwDbgInfo_debug dbg_op = NULL;

  const int max_shared_mem_size = hsail_get_agent_binary_shmem_max_size();

  if (payload == NULL)
    {
      /* return the cached dbgInfo there exists */
      return gs_DbgInfo;
    }

  gdb_assert(payload->m_Notification == HSAIL_NOTIFY_NEW_BINARY);

  uiout = current_uiout;
  /* Shared memory buffer pointer*/

  dbg_op = NULL;
  if(hsail_is_debug_facilities_loaded())
    {
      /* A copy of the shared memory segment )*/
      void* dbe_binary = NULL;
      size_t dbe_binary_size = 0;
      int shmid = -1;

      hsail_segment_update_loadmap();

      /*1M used is hard wired for now*/
      shmid = shmget(hsail_get_agent_binary_shmem_key(), max_shared_mem_size, 0666);

      if (shmid <= 0)
        {
          ui_out_text(uiout, "GDB: HwDbgFacilities init: shmid is invalid\n");
        }

      gdb_assert(shmid > 0);

      /* Get shm pointer */
      pShm = (int*)shmat(shmid, NULL, 0);

      if (pShm == NULL)
        {
          ui_out_text(uiout, "GDB: HwDbgFacilities init: pShm is NULL\n");
        }

      gdb_assert(pShm != NULL);

      dbe_binary_size = ((size_t*)pShm)[0];

      gdb_assert(dbe_binary_size > 0 && dbe_binary_size < max_shared_mem_size);

      dbe_binary = xmalloc(dbe_binary_size);

      gdb_assert(dbe_binary != NULL);

      memcpy(dbe_binary,(size_t*)pShm+1,dbe_binary_size);


      /* Uncomment this call if you need to save the binary to the file
      hsail_breakpoint_save_binary_to_file(dbe_binary_size, dbe_binary);
      */

      /* Attempt to initialize as a HSAIL backend binary*/
      dbg_op = hwdbginfo_init_with_hsa_1_0_binary(dbe_binary,
                                                  dbe_binary_size,
                                                  &errout_twolevel);

      /* Keep this printf here as a reminder for a
       * quick way to check that the IPC happened correctly*/
      /*
      int i = 0;
      for(i = 0; i<10;i++)
        {
          printf("%d \t %d\n",i,*((int*)dbe_binary + i));
        } */

      /* If we get a no HL binary, return code, we try to initialize as a single level binary */
      if (errout_twolevel == HWDBGINFO_E_NOHLBINARY)
        {
          /*
          dbg_op = hwdbginfo_init_with_single_level_binary(dbe_binary,
                                                           dbe_binary_size,
                                                           &errout_onelevel);
          */
          dbg_op = NULL;
        }

      /*
       * In the near future, debug facilities needs to be able to tell the difference
       * between an incomplete 2 level code object and a complete 1 level code object.
       * Since we dont support debugging LC for 1.3, this is not a big issue.
       *  */

      /* If we have a single level binary, thats good, we dont need to check the
       * two level return code */
      if (errout_twolevel == HWDBGINFO_E_NOHLBINARY && errout_onelevel == HWDBGINFO_E_SUCCESS)
        {
          fflush(stdout);
        }
      else if (errout_twolevel != HWDBGINFO_E_SUCCESS )
        {
          /* HwDbgFacilities init: Called DebugFacilities Incorrectly.
           * We can add more detailed messages such as low-level dwarf or high level dwarf missing in the future
           * */
          ui_out_text(uiout, "[ROCm-gdb]: The code object for the current dispatch does not contain debug information\n");
          fflush(stdout);

          dbg_op = NULL;

        }


      /* Test function to print all the mapped addresses and line numbers */
      /* hsail_dbginfo_test_all_mapped_addrs(dbg_op); */

      /* We can clear the dbe_binary buffer once we have initialized HWDbgFacilities */
      if (dbe_binary != NULL)
        {
          free_current_contents(&dbe_binary);
        }


      /* Get the kernel source, only if the 2 level initialization was good*/
      if (errout_twolevel == HWDBGINFO_E_SUCCESS)
        {
          if (!hsail_dbginfo_init_source_buffer(dbg_op))
          {
            ui_out_text(uiout, "[ROCm-gdb]: HwDbgFacilities get hsail text error\n");
          }
        }

      /* Detach shared memory */
      if (shmdt(pShm) == -1)
        {
          ui_out_text(uiout, "GDB: HwDbgFacilities init: Error detaching shm\n");
        }

    }

  /* cache the dgbInfo */
  gs_DbgInfo = dbg_op;

  return gs_DbgInfo;
  /*
   * This function's caller will use the returned context to query the
   * Debug Facilities API */
}

void hsail_free_hwdbginfo(void)
{
  if (gs_DbgInfo == NULL)
    {
      return;
    }
  else
    {
      gdb_assert(gs_DbgInfo!= NULL);

      /* Release the debug info handle: */
      hwdbginfo_release_debug_info(&gs_DbgInfo);

      gs_DbgInfo = NULL;

      if (active_kernel_src_file_path != NULL)
        {
          xfree(active_kernel_src_file_path);
          active_kernel_src_file_path =  NULL;
        }
    }
}
