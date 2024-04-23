#define _GNU_SOURCE
#define __ARM_ARCH_8A
#define FPGA_MANAGER_FW "/sys/class/fpga_manager/fpga0/firmware"
#define FPGA_MANAGER_UPLOAD_FILE "/lib/firmware/blink_hps.rbf"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sysexits.h>

#include <katcp.h>
#include <katcl.h>
#include <katpriv.h>
#include <netc.h>

#include <zlib.h>
#include <fcntl.h>

#include "tcpborphserver3.h"
#include "loadbof.h"
#include "tg.h"

#define MTU               1024*64

#define UPLOAD_LABEL      "upload"

#define UPLOAD_TIMEOUT    30
#define UPLOAD_PORT       7147

#define FPG_HEADER 589377378
#define BOF_HEADER 423776070

struct katcp_dispatch *g_client_dispatch = NULL; 

/*****************************************************************************************/

// Check for an Intel device
int is_intel_fpga()
{
    struct stat st;

    // First check sysfs
    if (stat("/sys/class/fpga_manager/fpga0", &st) == 0) {
        return 1;
    }

    // Fall back to device tree check
    FILE *f = fopen("/proc/device-tree/compatible", "rb");
    if (f) {
        char buffer[256];
        fread(buffer, 1, sizeof(buffer)-1, f);
        fclose(f);
        buffer[sizeof(buffer)-1] = '\0';
        if (strstr(buffer, "altr,socfpga"))
            return 1;
    }

    return 0; // Otherwise assume not Intel
}

// Check for a gzip file by looking at the first two characters
int is_gzipped_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0; // Assume not gzipped if can't open
    }

    unsigned char magic[2];
    if (fread(magic, 1, 2, f) != 2) {
        fclose(f);
        return 0;
    }
    fclose(f);

    return (magic[0] == 0x1f && magic[1] == 0x8b);
}


void destroy_port_data_tbs(struct katcp_dispatch *d, struct tbs_port_data *pd, int error)
{
  if(pd == NULL){
    return;
  }

  if(pd->t_fd > 0){
    close(pd->t_fd);
    pd->t_fd = (-1);
  }

  pd->t_port = 0;

  if(pd->t_name){

    switch(pd->t_del){
      case  TBS_DEL_NEVER  :
        break;
      case  TBS_DEL_ERROR  :
        if(error == 0){
          break;
        }
      case  TBS_DEL_ALWAYS :
        if(unlink(pd->t_name) < 0){
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to remove temporary file %s: %s", pd->t_name, strerror(errno));
        }
        break;
      /* default ... */
    }

    free(pd->t_name);
    pd->t_name = NULL;
  }

  free(pd);
}

struct tbs_port_data *create_port_data_tbs(struct katcp_dispatch *d, char *file, int port, int program, unsigned int expected, unsigned int timeout, unsigned int type, unsigned int delete)
{
  struct tbs_port_data *pd;

  if(file == NULL){
    return NULL;
  }

  pd = malloc(sizeof(struct tbs_port_data));

  if(port <= 0){
    pd->t_port = UPLOAD_PORT;
  } else {
    pd->t_port = port;
  }

  if(timeout <= 0){
    pd->t_timeout = UPLOAD_TIMEOUT;
  } else {
    pd->t_timeout = timeout;
  }

  pd->t_program = program;
  pd->t_expected = expected;

  pd->t_fd = (-1);
  pd->t_name = NULL;

  pd->t_type = type;
  pd->t_del = delete;

  pd->t_name = strdup(file);

  char temp_name[PATH_MAX];
  snprintf(temp_name, sizeof(temp_name), "/lib/firmware/%s", pd->t_name);
  free(pd->t_name);
  
  // Save the short name before modifying pd->t_name
  char *short_name = strdup(file);   // <- save "blink_hps.rbf" or "gateware"
  //pd->t_name = strdup(temp_name);
  if (file[0] == '/') {
    pd->t_name = strdup(file);  // absolute path, keep as-is
  } else {
    char temp_name[PATH_MAX];
    snprintf(temp_name, sizeof(temp_name), "/lib/firmware/%s", file);
    pd->t_name = strdup(temp_name);
  }

  if(pd->t_name == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "allocation failure while duplicating %s", file);
    destroy_port_data_tbs(d, pd, 1);
    return NULL;
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "using file %s", pd->t_name);

  pd->t_fd = open(pd->t_name, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR | S_IXUSR);
  if(pd->t_fd < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to open file %s: %s", pd->t_name, strerror(errno));
    destroy_port_data_tbs(d, pd, 1);
    return NULL;
  }

  return pd;
#undef BUFFER
}

/****************************************************************************/

int subprocess_upload_tbs(struct katcl_line *l, void *data)
{
    struct tbs_port_data *pd;
    int lfd, nfd, rr, wr, have;
    unsigned char buf[MTU];
    unsigned int count = 0;
    gzFile gfd;
    FILE *out = NULL;
    FILE *fpga_man = NULL;
    char compressed_path[PATH_MAX];
    char decompressed_path[PATH_MAX];
    char firmware_file[PATH_MAX];
    int skipping_metadata = 1;

    pd = data;
    fprintf(stderr, "DEBUG: subprocess_upload_tbs() called\n");

    if (!pd) {
        sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "no state supplied");
        return -1;
    }

    fprintf(stderr, "\tDEBUG: Calling net_listen\n");
    fprintf(stderr, "\tDEBUG: net_listen about to bind to port %d\n", pd->t_port);
    lfd = net_listen(NULL, pd->t_port, 0);
    if (lfd < 0) {
      fprintf(stderr, "\tDEBUG: net_listen failed: %s\n", strerror(errno));  
      sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "net_listen failed: %s", strerror(errno));
        return -1;
    }

    fprintf(stderr, "\tDEBUG: Calling signal\n");
    signal(SIGALRM, SIG_DFL);
    fprintf(stderr, "\tDEBUG: Calling alarm\n");
    alarm(pd->t_timeout);

    fprintf(stderr, "\tDEBUG: Calling accept\n");
    nfd = accept(lfd, NULL, 0);
    fprintf(stderr, "\tDEBUG: Calling close\n");
    close(lfd);
    if (nfd < 0) {
        sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "accept failed: %s", strerror(errno));
        return -1;
    }

    fprintf(stderr, "\tDEBUG: Opening compressed data\n");
    gfd = gzdopen(nfd, "r");
    if (!gfd) {
        close(nfd);
        sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "gzdopen failed: %s", strerror(errno));
        return -1;
    }

    //snprintf(compressed_path, sizeof(compressed_path), "/lib/firmware/%s", basename(pd->t_name));
    /*
    if (is_intel_fpga()) {
      fprintf(stderr, "\tDEBUG: Intel FPGA detected — renaming uploaded file to canonical name\n");       
     
      const char *original_path = pd->t_name;
      const char *canonical_path = "/lib/firmware/tcpborphserver.fpg";

      if (rename(original_path, canonical_path) != 0) {
        fprintf(stderr, "\t\tDEBUG: Failed to rename to canonical path\n");
        perror("rename to canonical .fpg name failed");
        gzclose(gfd);
        close(nfd);    
        return -1;
      }

      fprintf(stderr, "\tDEBUG: Renamed %s to %s\n", original_path, canonical_path);

      free(pd->t_name);  // Prevent memory leak
      pd->t_name = strdup(canonical_path);  // So later code uses the right name
      gzclose(gfd);
      close(nfd);
      //trigger_notice_katcp(g_client_dispatch, TBS_RAMFILE_PATH);
      trigger_notice_katcp(g_client_dispatch, "/bin/kcpfpg_intel");
      return 0;
    }
    */
   if (is_intel_fpga()) {
    fprintf(stderr, "\tDEBUG: Intel FPGA detected — writing raw .fpg\n");

    const char *canonical_path = "/lib/firmware/tcpborphserver.fpg";

    out = fopen(canonical_path, "wb");
    if (!out) {
        fprintf(stderr, "\t\tDEBUG: Could not open canoncial path\n");
        perror("fopen canonical .fpg");
        close(nfd);
        return -1;
    }

    while ((rr = read(nfd, buf, MTU)) > 0) {
        if (fwrite(buf, 1, rr, out) != rr) {
            perror("fwrite canonical .fpg");
            fclose(out);
            close(nfd);
            return -1;
        }
        count += rr;
    }

    fclose(out);
    close(nfd);

    fprintf(stderr, "\tDEBUG: Wrote %u bytes to %s\n", count, canonical_path);

    free(pd->t_name);
    pd->t_name = strdup(canonical_path);

    // Now that full file exists, trigger the .fpg handler
    trigger_notice_katcp(g_client_dispatch, "/bin/kcpfpg_intel");
    return 0;
    }

    else {
        fprintf(stderr, "\tDEBUG: Xilinx/Other FPGA detected, saving file raw\n");

        out = fopen(compressed_path, "wb");
        if (!out) {
            perror("fopen raw output");
            gzclose(gfd);
            return -1;
        }

        while ((rr = gzread(gfd, buf, MTU)) > 0) {
            have = 0;
            do {
                wr = write(fileno(out), buf + have, rr - have);
                if (wr > 0) {
                    have += wr;
                } else if (errno != EAGAIN && errno != EINTR) {
                    perror("write raw output");
                    fclose(out);
                    gzclose(gfd);
                    return -1;
                }
            } while (have < rr);
            count += rr;
        }

        fclose(out);
        gzclose(gfd);

        snprintf(firmware_file, sizeof(firmware_file), "%s", basename(pd->t_name));
    }

    fprintf(stderr, "\tDEBUG: received %u bytes\n", count);
    sync_message_katcl(l, KATCP_LEVEL_DEBUG, UPLOAD_LABEL, "received %u bytes", count);

    if (pd->t_expected > 0 && pd->t_expected != count) {
        fprintf(stderr, "\tDEBUG: expected %u bytes but received %u \n", pd->t_expected, count);
        sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "expected %u bytes but received %u", pd->t_expected, count);
        return -1;
    }

    // Tell FPGA Manager what file to load
    fprintf(stderr, "\tDEBUG: Opened FPGA Manager\n");
    fpga_man = fopen(FPGA_MANAGER_FW, "w");
    if (!fpga_man) {
        perror("fopen FPGA_MANAGER_FW");
        return -1;
    }
    fprintf(fpga_man, "%s\n", firmware_file);
    fclose(fpga_man);
    fprintf(stderr, "\tDEBUG: Closed FPGA Manager\n");

    //send_katcp(l, KATCP_FLAG_FIRST | KATCP_FLAG_STRING | KATCP_FLAG_LAST, "#fpga", "ready");
    //fprintf(stderr, "DEBUG: Sent KATCP flags\n");

    alarm(0);
    fprintf(stderr, "\tDEBUG: Alarm called\n");
    //sync_message_katcl(l, KATCP_LEVEL_INFO, UPLOAD_LABEL, "fpga programming complete via FPGA manager");
    fprintf(stderr, "\tDEBUG: Leaving subprocess_upload_tbs\n");
    return 0;
}


int transfer_status_tbs(struct katcp_dispatch *d, struct katcp_notice *n)
{
  struct katcl_parse *px;
  char *inform, *status;

  log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "got something from job via notice %p", n);

  px = get_parse_notice_katcp(d, n);
  if(px == NULL){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "no message returned by completed task");
    return -1;
  }

  inform = get_string_parse_katcl(px, 0);
  if(inform == NULL){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "null parameter in status message");
    return -1;
  }

  if(strcmp(inform, KATCP_RETURN_JOB)){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "expected to see a return inform, got %s instead", inform);
    return -1;
  }

  status = get_string_parse_katcl(px, 1);
  if(status == NULL){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "null status field");
    return -1;
  }

  if(strcmp(status, KATCP_OK) != 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "transfer returned status %s", status);
    return -1;
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "transfer appears to have succeeded");

  return 0;
}

int upload_generic_resume_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: upload_generic_resume_tbs called with notice name = %s\n", n->n_name);
  int result;
  result = transfer_status_tbs(d, n);
  fprintf(stderr, "DEBUG: result from transfer status is %u\n", result);
  prepend_reply_katcp(d);
  append_string_katcp(d, KATCP_FLAG_LAST, (result == 0) ? KATCP_OK : KATCP_FAIL);

  resume_katcp(d);

  return 0;
}

int sane_port_tbs(struct katcp_dispatch *d, unsigned int port)
{
  if(port <= 1024){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unwilling to bind reserved port %d", port);
    return -1;
  }
  if(port > 0xffff){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "port %d too large to be valid", port);
    return -1;
  }

  return 0;
}

int detect_file_tbs(struct katcp_dispatch *d, char *name, int fd)
{
#define BUFFER 128
  int rfd, rr;
  char buffer[BUFFER];
  char bofmagic[4] = { 0x19, 'B', 'O', 'F' };

  if (name && strlen(name) > 4 && !strcmp(name + strlen(name) - 4, ".rbf")) {
    fprintf(stderr, "DEBUG: Discovered that this is an .rbf file\n");
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected rbf file by extension");
    return TBS_FORMAT_RBF;
  }

  /* TODO - use gzopen */
  fprintf(stderr, "DEBUG: Checking file type\n");
  if(fd < 0){
    if(name == NULL){
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "no file given to examine");
      return TBS_FORMAT_BAD;
    }

    rfd = open(name, O_RDONLY);
    if(rfd < 0){
      log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "unable to open %s: %s", name, strerror(errno));
      return TBS_FORMAT_BAD;
    }
  } else {
    if(lseek(fd, 0, SEEK_SET) < 0){
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "unable to seek to start: %s", strerror(errno));
      return TBS_FORMAT_BAD;
    }

    rfd = fd;
  }

  rr = read(rfd, buffer, BUFFER);

  if(rfd != fd){
    close(rfd);
  }

  if(rr < BUFFER){
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "read failed: %s", (rr < 0) ? strerror(errno) : "insufficient data");
    return TBS_FORMAT_BAD;
  }

  buffer[BUFFER - 1] = '\0';

  if(!strncmp(buffer, bofmagic, 4)){
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected bof file");
    fprintf(stderr, "DEBUG: Discovered that this is a .bin file\n");
    return TBS_FORMAT_BOF;
  }

  if(buffer[0] == KATCP_REQUEST){
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected fpg file without header");
    return TBS_FORMAT_FPG;
  }

  if(!strncmp(buffer, "#!", 2)){
    if(strstr(buffer, TBS_KCPFPG_EXE)){
      fprintf(stderr, "DEBUG: Discovered that this is an .fpg file\n");
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected fpg file");
      return TBS_FORMAT_FPG;
    }
  }

  fprintf(stderr, "DEBUG: Discovered that this is an unknown file\n");
  log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "unknown file format");

  return TBS_FORMAT_BAD;
#undef BUFFER
}


/****************************************************************************/

int upload_filesystem_complete_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: upload_filesystem_complete_tbs was called");
  struct tbs_port_data *pd;
  int result;

  pd = data;
  if(pd == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: no port data given to handler\n");
    abort();
#endif
    return -1;
  }

  fprintf(stderr, "DEBUG: upload_filesystem_complete_tbs got data");

  result = transfer_status_tbs(d, n);

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "transfer of %s %s", pd->t_name, (result < 0) ? "failed" : "succeeded");

  destroy_port_data_tbs(d, pd, (result < 0) ? 1 : 0);

  return 0;
}

int upload_filesystem_cmd(struct katcp_dispatch *d, int argc)
{
  fprintf(stderr, "DEBUG: upload_filesystem_cmd was called");
  struct katcp_dispatch *dl;
  struct katcp_job *j;
  struct katcp_url *url;
  struct tbs_port_data *pd;
  unsigned int port, timeout, expected;
  struct tbs_raw *tr;
  struct katcp_notice *nx;
  char *name, *buffer;
  int len, result;

  dl = template_shared_katcp(d);
  if(dl == NULL){
    return KATCP_RESULT_FAIL;
  }

  tr = get_mode_katcp(d, TBS_MODE_RAW);
  if(tr == NULL){
    return KATCP_RESULT_FAIL;
  }

  expected = 0;
  timeout = 0;
  port = UPLOAD_PORT;

  if(argc < 3){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "need a port and file name to save data");
    return KATCP_RESULT_INVALID;
  }

  port = arg_unsigned_long_katcp(d, 1);
  if(sane_port_tbs(d, port) < 0){
    return KATCP_RESULT_INVALID;
  }

  name = arg_string_katcp(d, 2);
  if(name == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to acquire first parameter");
    return KATCP_RESULT_FAIL;
  }

  if(argc > 3){
    expected = arg_unsigned_long_katcp(d, 3);
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "expected length is %u bytes", expected);
    if(argc > 4){
      timeout = arg_unsigned_long_katcp(d, 4);
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "user requested a timeout of %us", timeout);
    }
  }

  if(strchr(name, '/') || (name[0] == '.')){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "refusing to upload file containing path information");
    return KATCP_RESULT_FAIL;
  }

  len = strlen(name) + strlen(tr->r_bof_dir) + 1;
  buffer= malloc(len + 1);
  if(buffer == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to allocate %d bytes", len + 1);
    extra_response_katcp(d, KATCP_RESULT_FAIL, "allocation");
    return KATCP_RESULT_OWN;
  }

  result = snprintf(buffer, len + 1, "%s/%s", tr->r_bof_dir, name);
  if(result != len){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "major logic failure: expected %d from snprintf, got %d", len, result);
    extra_response_katcp(d, KATCP_RESULT_FAIL, "internal");
    free(buffer);
    return KATCP_RESULT_OWN;
  }

  nx = find_notice_katcp(d, buffer);
  if(nx){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "another upload to %s already seems in progress, halting this attempt", name);
    free(buffer);
    return KATCP_RESULT_FAIL;
  }
  
  fprintf(stderr, "DEBUG: Creating katcp notice\n");
  nx = create_notice_katcp(d, buffer, 0);
  if(nx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when upload completes");
    free(buffer);
    return KATCP_RESULT_FAIL;
  }

  pd = create_port_data_tbs(d, buffer, port, 0, expected, timeout, TBS_FORMAT_ANY, TBS_DEL_ERROR);
  free(buffer);

  if(pd == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "could not initialise upload port state");
    return KATCP_RESULT_FAIL;
  }

  /* added in the global space dl, so that it completes even if client goes away */
  if(add_notice_katcp(dl, nx, &upload_filesystem_complete_tbs, pd) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback for upload completion");
    return KATCP_RESULT_FAIL;
  }

  /* add to local connection d, to resume it */
  if(add_notice_katcp(d, nx, &upload_generic_resume_tbs, NULL) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback to resume command");
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  fprintf(stderr, "DEBUG: Running upload_to_ram_and_program logic for %s\n", pd->t_name);
  url = create_exec_kurl_katcp("upload");
  if (url == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: could not create kurl", __func__);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  j = find_job_katcp(dl, url->u_str);
  if (j){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "found job for %s", url->u_str);
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  j = run_child_process_tbs(dl, url, &subprocess_upload_tbs, pd, nx);
  if (j == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run child process");
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "awaiting transfer on port %d", pd->t_port);

  return KATCP_RESULT_PAUSE;
}

/******************************************************************************************/

int upload_bin_complete_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  struct tbs_port_data *pd;
  int result;

  pd = data;
  if(pd == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: no port data given to handler\n");
    abort();
#endif
    return -1;
  }

  result = transfer_status_tbs(d, n);

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "transfer of %s %s", pd->t_name, (result < 0) ? "failed" : "succeeded");

  destroy_port_data_tbs(d, pd, (result < 0) ? 1 : 0);

  if(result == 0){
    if(start_fpg_tbs(d) < 0){
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to set up data structures after bin image programming");
    }
  }

  return 0;
}

int upload_bin_cmd(struct katcp_dispatch *d, int argc)
{
  struct katcp_dispatch *dl;
  struct katcp_job *j;
  struct katcp_url *url;
  struct tbs_port_data *pd;
  unsigned int port, timeout, expected;
  struct tbs_raw *tr;
  struct katcp_notice *nx;

  dl = template_shared_katcp(d);
  if(dl == NULL){
    return KATCP_RESULT_FAIL;
  }

  tr = get_mode_katcp(d, TBS_MODE_RAW);
  if(tr == NULL){
    return KATCP_RESULT_FAIL;
  }

  if ((tr->r_lkey != NULL) && strcmp(tr->r_lkey, d->d_name)){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "device locked");
    return KATCP_RESULT_FAIL;
  }

  expected = 0;
  timeout = 0;
  port = UPLOAD_PORT;

  if(argc > 1){
    port = arg_unsigned_long_katcp(d, 1);
    if(sane_port_tbs(d, port) < 0){
      return KATCP_RESULT_INVALID;
    }
  }

  if(argc > 2){
    expected = arg_unsigned_long_katcp(d, 2);
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "expected length is %u bytes", expected);
    if(argc > 3){
      timeout = arg_unsigned_long_katcp(d, 3);
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "user requested a timeout of %us", timeout);
    }
  }

  nx = find_notice_katcp(d, TBS_FPGA_CONFIG);
  if(nx){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "another upload already seems in progress, halting this attempt");
    return KATCP_RESULT_FAIL;
  }

  if(stop_fpga_tbs(d) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to deprogram fpga");
    return KATCP_RESULT_FAIL;
  }

  nx = create_notice_katcp(d, TBS_FPGA_CONFIG, 0);
  if(nx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when upload completes");
    return KATCP_RESULT_FAIL;
  }

  /* program set to 0 as we want to disable pd->t_program logic */
  pd = create_port_data_tbs(d, TBS_FPGA_CONFIG, port, 1, expected, timeout, TBS_FORMAT_BIN, TBS_DEL_NEVER);

  if (pd == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "could not create port data for bitstream upload");
    return KATCP_RESULT_FAIL;
  }

  /* added in the global space dl, so that it completes even if client goes away */
  if(add_notice_katcp(dl, nx, &upload_bin_complete_tbs, pd) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback for upload completion");
    return KATCP_RESULT_FAIL;
  }

  /* add to local connection d, to resume it */
  if(add_notice_katcp(d, nx, &upload_generic_resume_tbs, NULL) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback to resume command");
#if 1 /* we need to cancel notice somehow ? */
    destroy_port_data_tbs(d, pd, 1);
#endif
    return KATCP_RESULT_FAIL;
  }

  url = create_exec_kurl_katcp("upload");
  if (url == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: could not create kurl", __func__);
#if 1 /* we need to cancel notice somehow ? */
    destroy_port_data_tbs(d, pd, 1);
#endif
    return KATCP_RESULT_FAIL;
  }

  j = find_job_katcp(dl, url->u_str);
  if (j){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "found job for %s", url->u_str);
    destroy_kurl_katcp(url);
#if 1 /* we need to cancel notice somehow ? */
    destroy_port_data_tbs(d, pd, 1);
#endif
    return KATCP_RESULT_FAIL;
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "About to dislodge child process for programming");

  j = run_child_process_tbs(dl, url, &subprocess_upload_tbs, pd, nx);
  if (j == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run child process");
    destroy_kurl_katcp(url);
#if 1 /* we need to cancel notice somehow ? */
    destroy_port_data_tbs(d, pd, 1);
#endif
    return KATCP_RESULT_FAIL;
  }

  /* WARNING: closes fd as soon as possible, device driver prefers it that way */
  if(pd->t_fd > 0){
    close(pd->t_fd);
    pd->t_fd = (-1);
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "awaiting transfer on port %d", pd->t_port);

  return KATCP_RESULT_PAUSE;
}

/******************************************************************************************/

int upload_program_complete_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: Entered upload_program_complete_tbs\n");
  fprintf(stderr, "\tDEBUG: upload_program_complete_tbs d = %p\n", (void*)d);
  struct tbs_port_data *pd;

  pd = data;
  if(pd == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: no port data given to handler\n");
    abort();
#endif
    return -1;
  }

  destroy_port_data_tbs(d, pd, 0);
  fprintf(stderr, "\tDEBUG: Got rid of port data\n");
  
  if (is_intel_fpga()) {
        fprintf(stderr, "\tDEBUG: Intel FPGA detected, sending !progremote ok\n");
        prepend_reply_katcp(g_client_dispatch);
        append_string_katcp(g_client_dispatch, KATCP_FLAG_LAST, KATCP_OK);
        fprintf(stderr, "\tDEBUG: d = %p before resume_katcp\n", (void*)d);
        //send_katcp(d, KATCP_FLAG_FIRST | KATCP_FLAG_LAST | KATCP_FLAG_STRING,  "!progremote", KATCP_OK);
        //send_katcp(d, KATCP_FLAG_FIRST | KATCP_FLAG_LAST | KATCP_FLAG_STRING,  "!progremote", KATCP_OK);
        //fprintf(stderr, "\tDEBUG: Sent !progremote ok — now calling resume_katcp\n");
        //fprintf(stderr, "\tDEBUG: Sent !progremote ok\n");

        fprintf(stderr, "\tDEBUG: Called resume_katcp(), command should now complete\n");
        //send_katcp(g_client_dispatch, KATCP_FLAG_FIRST | KATCP_FLAG_LAST | KATCP_FLAG_STRING, "#fpga", "ready"); 
        send_katcp(g_client_dispatch,
          KATCP_FLAG_FIRST | KATCP_FLAG_STRING, "#fpga",
          KATCP_FLAG_LAST  | KATCP_FLAG_STRING, "ready");
      
        resume_katcp(g_client_dispatch);

        //return KATCP_RESULT_OK;
    }

  return 0;
}

int upload_program_partial_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  //fprintf(stderr, "DEBUG: Called upload program partial\n");
  struct tbs_port_data *pd;
  struct bof_state *bs;
  struct katcp_job *j;
  struct katcp_notice *nx;
  struct katcp_dispatch *dl;
  int type, result;
  char *argv[3];

  pd = data;
  fprintf(stderr, "DEBUG: ENTERED upload_program_partial_tbs, t_name = %s\n", pd->t_name);
  fprintf(stderr, "\tDEBUG: Checking for null data\n");

  if(pd == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: no port data given to handler\n");
    abort();
#endif
    return -1;
  }
 
  fprintf(stderr, "\tDEBUG: Checking for null name\n");

  if(pd->t_name == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: null name in upload data structure\n");
    abort();
#endif
    return -1;
  }

  fprintf(stderr, "\tDEBUG: Checking file type of %s\n", pd->t_name);

  type = detect_file_tbs(d, pd->t_name, pd->t_fd);
  fprintf(stderr, "\tDEBUG: Detected file type is %d\n", type);
  switch(type){
    case TBS_FORMAT_BOF :
    
      fprintf(stderr, "\tDEBUG: Calling transfer status tbs for BOF file\n");

      result = transfer_status_tbs(d, n);
      if(result < 0){
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }
    
      fprintf(stderr, "\tDEBUG: Finding katcp notice\n");
      nx = find_notice_katcp(d, TBS_KCPFPG_PATH);
      if(nx){
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "not proceeding with programming as another instance is already in flight");
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }
    
      fprintf(stderr, "\tDEBUG: Checking if stop fpga tbs\n");
    
      if(stop_fpga_tbs(d) < 0){
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }
    

      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "processing upload as bof format");
      result = (-1);

      bs = open_bof(d, pd->t_name);
      if(bs){
        log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "managed to open %s", pd->t_name);
        result = start_bof_tbs(d, bs);
        close_bof(d, bs);
      }

      destroy_port_data_tbs(d, pd, (result < 0) ? 1 : 0);

      return result;

    case TBS_FORMAT_FPG :
      fprintf(stderr, "\t\tDEBUG: Dispatch name is %s\n", pd->t_name);
      if (is_intel_fpga()) {
        fprintf(stderr, "\t\tDEBUG: Intel FPGA detected, handling .fpg via kcpfpg_intel\n");
    
        dl = template_shared_katcp(d);
        if (!dl) {
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
        }
    
        struct katcp_notice *nx = create_notice_katcp(d, "/bin/kcpfpg_intel", 0);
        if (!nx) {
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notice for kcpfpg_intel");
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
        }
    
        if (add_notice_katcp(d, nx, &upload_program_complete_tbs, pd) < 0) {
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback for kcpfpg_intel completion");
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
        }
    
        char *argv[] = { "/bin/kcpfpg_intel", pd->t_name, NULL };
        fprintf(stderr, "\t\tDEBUG: Starting child job: %s %s\n", argv[0], argv[1]);
    
        struct katcp_job *j = process_name_create_job_katcp(dl, "/bin/kcpfpg_intel", argv, nx, NULL);
        if (!j) {
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "failed to launch kcpfpg_intel");
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
        }
    
        return 0;
      }
      else {
        result = transfer_status_tbs(d, n);
        if(result < 0){
          destroy_port_data_tbs(d, pd, 1);
          return -1;
        }
      
        fprintf(stderr, "\t\tDEBUG: Finding katcp notice\n");
        nx = find_notice_katcp(d, TBS_KCPFPG_PATH);
        if(nx){
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "not proceeding with programming as another instance is already in flight");
          destroy_port_data_tbs(d, pd, 1);
          return -1;
        }
      
        fprintf(stderr, "\t\tDEBUG: Checking if stop fpga tbs\n");
      
        if(stop_fpga_tbs(d) < 0){
          destroy_port_data_tbs(d, pd, 1);
          return -1;
        }
        fprintf(stderr, "\t\tDEBUG: Found an .fpg file\n");

        log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "assuming new fpg format for %s", pd->t_name);
        
        fprintf(stderr, "\t\tDEBUG: Creating shared katcp template\n");

        dl = template_shared_katcp(d);
        if(dl == NULL){
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
        }
      
        fprintf(stderr, "\t\tDEBUG: Creating katcp notice\n");

        nx = create_notice_katcp(d, TBS_KCPFPG_PATH, 0);
        if(nx == NULL){
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when %s completes", TBS_KCPFPG_PATH);
          destroy_port_data_tbs(d, pd, 1);
          return -1;
        }

        fprintf(stderr, "\t\tDEBUG: Checking for completion of upload program\n");

        if(add_notice_katcp(d, nx, &upload_program_complete_tbs, pd) < 0){
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback to resume command");
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
        }

        argv[0] = TBS_KCPFPG_PATH;
        argv[1] = pd->t_name;
        argv[2] = NULL;
        
        fprintf(stderr, "\t\tDEBUG: job name = %s\n", argv[0]);  // Must be tcpborphserver.bin
        j = process_name_create_job_katcp(dl, TBS_KCPFPG_PATH, argv, nx, NULL);
        if (j == NULL){
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run %s child process", TBS_KCPFPG_PATH);
  #if 1
          destroy_port_data_tbs(d, pd, 1);
  #endif
          return -1;
        }

        return 0;
      }  
    /*
    case TBS_FORMAT_RBF:
      fprintf(stderr, "\t\tDEBUG: Found an .rbf file\n");

      dl = template_shared_katcp(d);
      if(dl == NULL){
        destroy_port_data_tbs(d, pd, 1);
        return KATCP_RESULT_FAIL;
      }

      // Match dispatch name to expected path
      const char *dispatch_name = TBS_KCPFPG_PATH;

      fprintf(stderr, "\t\tDEBUG: Creating katcp notice for %s\n", dispatch_name);
      nx = create_notice_katcp(d, dispatch_name, 0);
      if(nx == NULL){
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic for %s", dispatch_name);
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }

      if(add_notice_katcp(d, nx, &upload_program_complete_tbs, pd) < 0){
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback");
        destroy_port_data_tbs(d, pd, 1);
        return KATCP_RESULT_FAIL;
      }

      argv[0] = TBS_KCPFPG_PATH;
      argv[1] = pd->t_name;
      argv[2] = NULL;
      fprintf(stderr, "\t\tDEBUG: Child will exec: %s %s\n", argv[0], argv[1]);
      fprintf(stderr, "\t\tDEBUG: Starting job %s\n", argv[0]);
      j = process_name_create_job_katcp(dl, dispatch_name, argv, nx, NULL);
      if(j == NULL){
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to start child job");
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }

      fprintf(stderr, "\t\tDEBUG: Calling transfer status tbs\n");
      result = transfer_status_tbs(d, nx);
      if(result < 0){
        fprintf(stderr, "\t\t\tDEBUG: Result < 0\n");
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }
      fprintf(stderr, "\t\tDEBUG:Returning from upload_program_partial_tbs\n");
      return 0;
    */  
    /*
    case TBS_FORMAT_RBF:
      fprintf(stderr, "\tDEBUG: Detected Intel .rbf upload. No Further programming required\n");
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected Intel .rbf upload, no further programming required");
      
      dl = template_shared_katcp(d);
      if (!dl) {
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
      }
      fprintf(stderr, "\t\tDEBUG: creating notice for progremote\n");
      fprintf(stderr, "\t\tDEBUG: dl = %p, d = %p\n", (void*)dl, (void*)d);
      
      
      nx = create_notice_katcp(d, "progremote", 0);  // tie this job to ?progremote
      if (!nx) {
          destroy_port_data_tbs(d, pd, 1);
          return -1;
      }

      if (add_notice_katcp(d, nx, &upload_program_complete_tbs, pd) < 0) {
          destroy_port_data_tbs(d, pd, 1);
          return KATCP_RESULT_FAIL;
      }
      
      // Spawn a dummy child process that finishes immediately
      char *argv[] = { "/bin/true", NULL };  // dummy success command
      fprintf(stderr, "\t\tDEBUG: creating dummy child job\n");
      j = process_name_create_job_katcp(d, "progremote", argv, nx, NULL);
      if (!j) {
          destroy_port_data_tbs(d, pd, 1);
          return -1;
      }

      return 0;    
    */
    default :
      fprintf(stderr, "DEBUG: Katcp error - was sent unusable or invalid format\n");
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "was sent unusable or invalid format");
      destroy_port_data_tbs(d, pd, 1);
    return -1;
  }

}

#if 0
int progremote_tbs(struct katcl_line *l, void *data)
{
  struct tbs_port_data *pd;
  int lfd, nfd, rr, wr, have;
  unsigned char buf[MTU];
  unsigned int count;
  gzFile gfd;
  char *argv[2];
  int check_header = 0;
  int i;
  char header[4];
  uint32_t int_hdr = 0;

  pd = data;

  if (pd == NULL){
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "no state supplied to subordinate logic");
    return -1;
  }

  lfd = net_listen(NULL, pd->t_port, 0);
  if (lfd < 0){
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "unable to bind port %d: %s", pd->t_port, strerror(errno));
    return -1;
  }

  signal(SIGALRM, SIG_DFL);
  alarm(pd->t_timeout);

  nfd = accept(lfd, NULL, 0);
  close(lfd);

  if (nfd < 0){
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "accept on port %d failed: %s", pd->t_port, strerror(errno));
    return -1;
  }

  gfd = gzdopen(nfd, "r");
  if(gfd == NULL){
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "gzdopen fail %s", strerror(errno));
    return -1;
  }

  count = 0;

  for (;;){
    rr = gzread(gfd, buf, MTU);
    if (rr == 0){
      break;
    } else if (rr < 0){
      sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "read failed while receiving bof file: %s", strerror(errno));
      close(nfd);
      return -1;
    }

    have = 0;
    do {
      wr = write(pd->t_fd, buf + have, rr - have);
      switch(wr){

        case -1:
          switch(errno){
            case EAGAIN:
            case EINTR:
              break;
            default:
              sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "saving of bof file failed: %s", strerror(errno));
              close(nfd);
              return -1;
          }
          break;

        case 0:
          sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "unexpected zero write");
          close(nfd);
          return -1;

        default:
          have += wr;
#if 0
          sync_message_katcl(l, KATCP_LEVEL_DEBUG, NULL, "%s: wrote %d bytes to parent", __func__, wr);
#endif
          break;
      }
      if(check_header == 0){
        for(i = 0; i < 4; i++){
          header[i] = buf[i];
        }
        int_hdr = (header[0] << 24 | header[1] << 16 | header[2] << 8 | header[3]);


        sync_message_katcl(l, KATCP_LEVEL_DEBUG, UPLOAD_LABEL, "integer header is:%u", int_hdr);
        if(int_hdr == BOF_HEADER){
          sync_message_katcl(l, KATCP_LEVEL_DEBUG, UPLOAD_LABEL, "BOF HEADER matched");
          pd->t_type = 1;

        }else if(int_hdr == FPG_HEADER){
          sync_message_katcl(l, KATCP_LEVEL_DEBUG, UPLOAD_LABEL, "FPG HEADER matched");
          pd->t_type = 2;

        }else{
          sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "NO HEADER matched");
          pd->t_type = 0;
          return -1;

        }
      }
      check_header = 1;
    } while(have < rr);

    count += rr;

    alarm(UPLOAD_TIMEOUT);
  }

  gzclose(gfd);
  close(nfd);

  sync_message_katcl(l, KATCP_LEVEL_INFO, UPLOAD_LABEL, "received file %s containing %u bytes", pd->t_name, count);

  if(pd->t_expected > 0){
    if(pd->t_expected != count){
      sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "expected %u bytes, received %u", pd->t_expected, count);
      return -1;
    }
  }
  alarm(0);

  if(pd->t_type == 2){
    /*Run execve on the /dev/shm file */
    argv[0] = TBS_KCPFPG_PATH;
    argv[1] = pd->t_name;
    argv[2] = NULL;
    sync_message_katcl(l, KATCP_LEVEL_DEBUG, UPLOAD_LABEL, "About to execve with arguments %s", argv[0]);

    /*execve only returns on error*/
    execve(TBS_KCPFPG_PATH, argv, NULL);

    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "unable to run execve %s (%s)", "/bin/kcpfpg", strerror(errno));

    destroy_katcl(l, 0);
    exit(EX_OSERR);

    return -1;
  }
  return 0;
}
#endif

#if 0
#define PROGREMOTE_NOTICE "progremote-upload"
#endif

int upload_program_cmd(struct katcp_dispatch *d, int argc)
{
  g_client_dispatch = d;
  fprintf(stderr, "DEBUG: upload_program_cmd called\n");
  fprintf(stderr, "\tDEBUG: upload_program_cmd d = %p\n", (void*)d);
  struct katcp_dispatch *dl;
  struct katcp_job *j;
  struct katcp_url *url;
  struct tbs_port_data *pd;
  unsigned int port, timeout, expected;
  struct tbs_raw *tr;
  struct katcp_notice *nx;

  dl = template_shared_katcp(d);
  if(dl == NULL){
    return KATCP_RESULT_FAIL;
  }

  tr = get_mode_katcp(d, TBS_MODE_RAW);
  if(tr == NULL){
    return KATCP_RESULT_FAIL;
  }

  if ((tr->r_lkey != NULL) && strcmp(tr->r_lkey, d->d_name)){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "device locked");
    return KATCP_RESULT_FAIL;
  }

  expected = 0;
  timeout = 0;
  port = UPLOAD_PORT;
  fprintf(stderr, "\tDEBUG: Listening on port %u\n", port);
  if(argc > 1){
    port = arg_unsigned_long_katcp(d, 1);
    if(sane_port_tbs(d, port) < 0){
      return KATCP_RESULT_INVALID;
    }
  }

  if(argc > 2){
    expected = arg_unsigned_long_katcp(d, 2);
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "expected length is %u bytes", expected);
    if(argc > 3){
      timeout = arg_unsigned_long_katcp(d, 3);
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "user requested a timeout of %us", timeout);
    }
  }

  nx = find_notice_katcp(d, TBS_RAMFILE_PATH);
  if(nx){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "another upload already seems in progress, halting this attempt");
    return KATCP_RESULT_FAIL;
  }

  stop_fpga_tbs(d);

  nx = create_notice_katcp(d, TBS_RAMFILE_PATH, 0);
  if(nx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when upload completes");
    return KATCP_RESULT_FAIL;
  }

  pd = create_port_data_tbs(d, TBS_RAMFILE_PATH, port, 0, expected, timeout, TBS_FORMAT_ANY, TBS_DEL_ALWAYS);
  if (pd == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: couldn't create port data", __func__);
    return KATCP_RESULT_FAIL;
  }

  /* added in the global space dl, so that it completes even if client goes away */
  if(add_notice_katcp(dl, nx, &upload_program_partial_tbs, pd) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback for upload completion");
    return KATCP_RESULT_FAIL;
  }

  url = create_exec_kurl_katcp("progremote");
  if (url == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: could not create kurl", __func__);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  j = find_job_katcp(dl, url->u_str);
  if (j){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "found job for %s", url->u_str);
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

#if 0
  j = run_child_process_tbs(dl, url, &progremote_tbs, pd, nx);
  if (j == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run child process");
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd);
    return KATCP_RESULT_FAIL;
  }
#endif
  if (is_intel_fpga()) {
    const char *canonical_fpg = "/lib/firmware/tcpborphserver.fpg";
    fprintf(stderr, "\tDEBUG: Intel FPGA — renaming %s to %s\n", pd->t_name, canonical_fpg);
    rename(pd->t_name, canonical_fpg);
    free(pd->t_name);
    pd->t_name = strdup(canonical_fpg);
  }
  j = run_child_process_tbs(dl, url, &subprocess_upload_tbs, pd, nx);
  if (j == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run child process");
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "awaiting transfer on port %d", pd->t_port);
  fprintf(stderr, "\tDEBUG: Determining flow path\n");
  if (is_intel_fpga()) {
    fprintf(stderr, "\tDEBUG: Intel FPGA detected, using FPGA Manager flow\n");
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "Intel FPGA, returning KATCP_RESULT_OK");

    return KATCP_RESULT_OK; 
  }

  fprintf(stderr, "\tDEBUG: Launching child process subprocess_upload_tbs\n");
  fprintf(stderr, "\tDEBUG: Xilinx FPGA detected, resuming normal direct flow\n");
  //log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "Non-Intel FPGA, assuming direct flow");
  return KATCP_RESULT_OK;
  

}


/* unused ************************************************************************/

#if 0
int upload_complete_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  struct tbs_port_data *pd;
  struct katcl_parse *p;
  char *inform, *status;
  int fd;
  struct bof_state *bs;

#if 0
  int fd, offset, wr, sr;
#endif

  pd = data;
  if (pd == NULL){
    return -1;
  }

  p = get_parse_notice_katcp(d, n);
  if(p == NULL){
    destroy_port_data_tbs(d, pd, 1);
    return 0;
  }

  inform = get_string_parse_katcl(p, 0);
  if(inform == NULL){
    destroy_port_data_tbs(d, pd, 1);
    return 0;
  }

#ifdef DEBUG
  fprintf(stderr, "%s: got inform %s\n", __func__, inform);
#endif

  if(strcmp(inform, KATCP_RETURN_JOB) != 0){
    destroy_port_data_tbs(d, pd, 1);
    return 0;
  }

  status = get_string_parse_katcl(p, 1);
  if(status == NULL){
    destroy_port_data_tbs(d, pd, 1);
    return 0;
  }

  if(strcmp(status, KATCP_OK) != 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "encountered %s on upload", status);
    destroy_port_data_tbs(d, pd, 1);
    return 0;
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "transfer completed for %s upload", pd->t_program ? "programmed" : "plain");

  if(pd->t_program){

    if (lseek(pd->t_fd, 0, SEEK_SET) < 0){
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable lseek begining of file");
      destroy_port_data_tbs(d, pd, 1);
      return 0;
    }

    if(stop_fpga_tbs(d) < 0){
      destroy_port_data_tbs(d, pd, 1);
      return 0;
    }

    fd = dup(pd->t_fd);
    if(fd < 0){
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable dup file descriptor: %s", strerror(errno));
      destroy_port_data_tbs(d, pd, 1);
      return 0;
    }

    bs = open_bof_fd(d, fd);
    if(bs == NULL){
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to initialise programming logic");
      destroy_port_data_tbs(d, pd, 1);
      return 0;
    }

    if(start_bof_tbs(d, bs) < 0){
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to program uploaded bof file");
      close_bof(d, bs);
      destroy_port_data_tbs(d, pd, 1);
      return 0;
    }

    close_bof(d, bs);
  }

  destroy_port_data_tbs(d, pd, 0);

  return 0;
}

/* !< this doesn't seem to be called */
int upload_cmd(struct katcp_dispatch *d, int argc)
{
  struct katcp_dispatch *dl;
  struct katcp_job *j;
  struct katcp_url *url;
  struct tbs_port_data *pd;
  unsigned int port, timeout, expected;
  struct tbs_raw *tr;
  struct katcp_notice *nx;

  dl = template_shared_katcp(d);
  if(dl == NULL){
    return KATCP_RESULT_FAIL;
  }

  tr = get_mode_katcp(d, TBS_MODE_RAW);
  if(tr == NULL){
    return KATCP_RESULT_FAIL;
  }

  expected = 0;
  timeout = 0;
  port = UPLOAD_PORT;

  if(argc > 1){
    port = arg_unsigned_long_katcp(d, 1);
    if(sane_port_tbs(d, port) < 0){
      return KATCP_RESULT_INVALID;
    }
  }

  if(argc > 2){
    expected = arg_unsigned_long_katcp(d, 2);
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "expected length is %u bytes", expected);
    if(argc > 3){
      timeout = arg_unsigned_long_katcp(d, 3);
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "user requested a timeout of %us", timeout);
    }
  }

  nx = find_notice_katcp(d, TBS_FPGA_CONFIG);
  if(nx){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "another upload already seems in progress, halting this attempt");
    return KATCP_RESULT_FAIL;
  }

  nx = create_notice_katcp(d, TBS_FPGA_CONFIG, 0);
  if(nx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when upload completes");
    return KATCP_RESULT_FAIL;
  }

  pd = create_port_data_tbs(d, NULL, port, 1, expected, timeout, TBS_FORMAT_ANY, TBS_DEL_ALWAYS);

  if (pd == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: couldn't create port data", __func__);
    return KATCP_RESULT_FAIL;
  }

  /* added in the global space dl, so that it completes even if client goes away */
  if(add_notice_katcp(dl, nx, &upload_complete_tbs, pd) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback for upload completion");
    return KATCP_RESULT_FAIL;
  }

  url = create_exec_kurl_katcp("upload");
  if (url == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: could not create kurl", __func__);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  j = find_job_katcp(dl, url->u_str);
  if (j){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "found job for %s", url->u_str);
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  j = run_child_process_tbs(dl, url, &subprocess_upload_tbs, pd, nx);
  if (j == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run child process");
    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "awaiting transfer on port %d", pd->t_port);

  status_fpga_tbs(d, TBS_FPGA_PROGRAMMED);

  tr->r_top_register = infer_fpga_range(d);

  if(map_raw_tbs(d) < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "Unable to map /dev/roach/mem");
    return -1;
  }

#if 0
  /* map raw does this */
  tr->r_fpga = TBS_FPGA_MAPPED;
#endif

  return KATCP_RESULT_OK;
}
#endif
