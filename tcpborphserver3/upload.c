#define _GNU_SOURCE

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
#define UPLOAD_PORT       7146

#define FPG_HEADER 589377378
#define BOF_HEADER 423776070

/*****************************************************************************************/
// Check for an Intel device
int is_intel_fpga()
{
    fprintf(stderr, "DEBUG: Inside is_intel_fpga\n");

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
    fprintf(stderr, "DEBUG: Inside is_gzipped_file\n");
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

int program_intel_rbf(char* fpg_file) {
    fprintf(stderr, "DEBUG: Inside program_intel_rbf\n");
    const char *input_fpg = fpg_file;
    struct stat st;

    if (stat(input_fpg, &st) != 0) {
        perror("stat on input .fpg failed");
        return 1;
    }

    fprintf(stderr, "DEBUG KCPFPG_INTEL: launching on: %s\n", input_fpg);
    fprintf(stderr, "DEBUG KCPFPG_INTEL: Input .fpg size: %lld bytes\n", st.st_size);

    char buf[MTU];
    char *base = basename((char *)input_fpg);
    char compressed_path[PATH_MAX];
    snprintf(compressed_path, sizeof(compressed_path), "/tmp/%s_payload.gz", base);

    FILE *in = fopen(input_fpg, "rb");
    if (!in) {
        perror("fopen input .fpg");
        return 1;
    }

    FILE *compressed_out = fopen(compressed_path, "wb");
    if (!compressed_out) {
        perror("fopen compressed output");
        fclose(in);
        return 1;
    }

    // Skip ASCII header until we find ?quit
    char line[MTU];
    long payload_offset = -1;
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "?quit", 5) == 0) {
            payload_offset = ftell(in); // offset after newline following ?quit
            break;
        }
    }

    if (payload_offset < 0) {
        fprintf(stderr, "ERROR: ?quit not found in .fpg file\n");
        fclose(in);
        fclose(compressed_out);
        return 1;
    }

    fprintf(stderr, "DEBUG KCPFPG_INTEL: Found ?quit at offset %ld\n", payload_offset);

    // Copy raw gzipped payload
    fseek(in, payload_offset, SEEK_SET);
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, r, compressed_out);
    }

    fclose(in);
    fclose(compressed_out);

    // Now decompress the extracted gzip stream
    gzFile compressed = gzopen(compressed_path, "rb");
    if (!compressed) {
        perror("gzopen decompress");
        return 1;
    }

    char decompressed_path[PATH_MAX];
    snprintf(decompressed_path, sizeof(decompressed_path), "/tmp/%s_decompressed.rbf", base);
    FILE *decompressed = fopen(decompressed_path, "wb");
    if (!decompressed) {
        perror("fopen decompressed output");
        gzclose(compressed);
        return 1;
    }

    int rr;
    while ((rr = gzread(compressed, buf, MTU)) > 0) {
        if (fwrite(buf, 1, rr, decompressed) != (size_t)rr) {
            perror("fwrite decompressed");
            fclose(decompressed);
            gzclose(compressed);
            return 1;
        }
    }

    if (rr < 0) {
        int errnum;
        const char *errmsg = gzerror(compressed, &errnum);
        fprintf(stderr, "ERROR: gzread failed during decompression: %s\n", errmsg);
    }

    fclose(decompressed);
    gzclose(compressed);
    unlink(compressed_path);

    // Copy to /lib/firmware
    const char *firmware_dst = "/lib/firmware/tcpborphserver.rbf";
    FILE *src = fopen(decompressed_path, "rb");
    FILE *dst = fopen(firmware_dst, "wb");
    if (!src || !dst) {
        perror("fopen for copy to /lib/firmware");
        if (src) fclose(src);
        if (dst) fclose(dst);
        return 1;
    }

    while ((rr = fread(buf, 1, MTU, src)) > 0) {
        if (fwrite(buf, 1, rr, dst) != (size_t)rr) {
            perror("fwrite to /lib/firmware");
            fclose(src);
            fclose(dst);
            return 1;
        }
    }

    fclose(src);
    fclose(dst);
    unlink(decompressed_path);

    struct stat rbfs;
    if (stat(firmware_dst, &rbfs) == 0) {
        fprintf(stderr, "DEBUG KCPFPG_INTEL: Final .rbf size: %lld bytes\n", rbfs.st_size);
    } else {
        perror("stat final .rbf");
    }

    // Trigger FPGA Manager
    FILE *fw = fopen("/sys/class/fpga_manager/fpga0/firmware", "w");
    if (!fw) {
        perror("open firmware sysfs");
        return 1;
    }

    if (fprintf(fw, "tcpborphserver.rbf") < 0) {
        perror("write firmware name");
        fclose(fw);
        return 1;
    }

    fclose(fw);
    fprintf(stderr, "DEBUG KCPFPG_INTEL: Successfully programmed FPGA\n");
    return 0;
}

void destroy_port_data_tbs(struct katcp_dispatch *d, struct tbs_port_data *pd, int error)
{
  fprintf(stderr, "DEBUG: Inside destroy_port_data_tbs\n");

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
  fprintf(stderr, "DEBUG: Inside create_port_data_tbs\n");

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
  fprintf(stderr, "DEBUG: setting pd->t_name = %s\n", file);
  if(pd->t_name == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "allocation failure while duplicating %s", file);
    destroy_port_data_tbs(d, pd, 1);
    return NULL;
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "using file %s", pd->t_name);
  fprintf(stderr, "\tDEBUG: Using file %s\n", pd->t_name);

  pd->t_fd = open(pd->t_name, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR | S_IXUSR);
  //pd->t_fd = open(pd->t_name, O_CREAT | O_TRUNC | O_WRONLY, 0666);
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
  fprintf(stderr, "DEBUG: Inside subprocess_upload_tbs\n");

  /* TODO: once kcpfpg does gzopen, this should only decompress if format is BIN ? */

  struct tbs_port_data *pd;
  int lfd, nfd, rr, wr, have;
  unsigned char buf[MTU];
  unsigned int count;
  gzFile gfd;
#ifdef __ARM_ARCH_8A
  FILE *fpga_man;
#endif

  pd = data;

  if (pd == NULL){
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "no state supplied to subordinate logic");
    return -1;
  }

  lfd = net_listen(NULL, pd->t_port, 0);
  fprintf(stderr, "DEBUG: Listening on port %d\n", pd->t_port);
  if (lfd < 0){
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "unable to bind port %d: %s", pd->t_port, strerror(errno));
    return -1;
  }

  fprintf(stderr, "DEBUG: Checking t_del %d\n", pd->t_del);
  signal(SIGALRM, SIG_DFL);
  alarm(pd->t_timeout);

  nfd = accept(lfd, NULL, 0);
  close(lfd);

  if(nfd < 0){
    fprintf(stderr, "DEBUG: Accept on port %d failed: %s\n", pd->t_port, strerror(errno));
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "accept on port %d failed: %s", pd->t_port, strerror(errno));
    return -1;
  }
  fprintf(stderr, "DEBUG: Calling gzdopen on fd %d\n", nfd);
  gfd = gzdopen(nfd, "r");
  /*
  int test = gzread(gfd, buf, 1);
  if (test < 0) {
      int errnum;
      const char *errmsg = gzerror(gfd, &errnum);
      fprintf(stderr, "DEBUG: gzread error after open: %s\n", errmsg);
      return -1;
  }
  */    
  if(gfd == NULL){
    close(nfd);
    fprintf(stderr, "DEBUG: gzdopen on network data stream failed: %s\n", strerror(errno));
    sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "gzdopen on network data stream failed: %s", strerror(errno));
    return -1;
  }

  count = 0;
  off_t total_written = 0;
  for (;;){
    rr = gzread(gfd, buf, MTU);
    if (rr == 0){
      break;
    } else if (rr < 0){
      int gzerrnum;
      const char *gzerrmsg = gzerror(gfd, &gzerrnum);
      fprintf(stderr, "DEBUG: gzread failed: %s (zlib code %d)\n", gzerrmsg, gzerrnum);
      sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "gzread failed: %s", gzerrmsg);
      //sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "read failed while receiving %s", strerror(errno));
      gzclose(gfd);
      return -1;
    }
    /*
    fprintf(stderr, "DEBUG: First few bytes being written:\n");
    for (int k = 0; k < 16; k++) {
        fprintf(stderr, " 0x%02x", buf[k]);
    }
    fprintf(stderr, "\n");
    */
    fprintf(stderr, "DEBUG: writing to pd->t_name = %s\n", pd->t_name);
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
              fprintf(stderr, "DEBUG: Saving of network stream to file failed: %s\n", strerror(errno));
              sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "saving of network stream to file failed: %s", strerror(errno));
              gzclose(gfd);
              return -1;
          }
          break;

        case 0:
          fprintf(stderr, "DEBUG: Unexpected zero write\n");
          sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "unexpected zero write");
          gzclose(gfd);
          return -1;

        default:
          have += wr;
#if 0
          sync_message_katcl(l, KATCP_LEVEL_DEBUG, NULL, "%s: wrote %d bytes to parent", __func__, wr);
#endif
          break;
      }
    } while(have < rr);

    count += rr;
    total_written += rr;
  // Trim to exact decompressed size
  if (ftruncate(pd->t_fd, total_written) < 0) {
      perror("ftruncate failed");
  }
#if 0
    sync_message_katcl(l, KATCP_LEVEL_INFO, NULL, "uploaded %d bytes", pd->t_rsize);
#endif

    alarm(UPLOAD_TIMEOUT);
  }
  int errnum;
  const char *errstr = gzerror(gfd, &errnum);
  fprintf(stderr, "DEBUG: gzerror after read: %s (code %d)\n", errstr, errnum);
  fsync(pd->t_fd);
  gzclose(gfd);
  
  fprintf(stderr, "DEBUG: Received file data of %u bytes\n", count);
  sync_message_katcl(l, KATCP_LEVEL_DEBUG, UPLOAD_LABEL, "received file data of %u bytes", count);

  if(pd->t_expected > 0){
    if(pd->t_expected != count){
      fprintf(stderr, "DEBUG: Expected %u bytes but received %u\n", pd->t_expected, count);
      sync_message_katcl(l, KATCP_LEVEL_ERROR, UPLOAD_LABEL, "expected %u bytes but received %u", pd->t_expected, count);
      return -1;
    }
  }

#ifdef USE_FPGA_MANAGER
  fprintf(stderr, "DEBUG: Using the FPGA Manager flow\n");
  /* Close file that we just finished writing to */
  close(pd->t_fd);
  fprintf(stderr, "DEBUG: Closed %s\n", pd->t_name);
  fprintf(stderr, "DEBUG: t_program is set to %d\n", pd->t_program);

  if(pd->t_program) {
    fprintf(stderr, "DEBUG: Programming device\n");
    FILE *fpga_man;
    fpga_man = fopen(FPGA_MANAGER_FW, "w");
    if(fpga_man == NULL){
      fprintf(stderr, "DEBUG: Unable to open %s: %s\n", FPGA_MANAGER_FW, strerror(errno));
      sync_message_katcl(l, KATCP_LEVEL_ERROR, NULL, "unable to open %s: %s", FPGA_MANAGER_FW, strerror(errno));
      //sync_message_katcl(l, KATCP_LEVEL_ERROR, NULL, "unable to open fpga manager flags");
      return -1;
    }

    const char *filename = strrchr(pd->t_name, '/');
    filename = filename ? filename + 1 : pd->t_name;
    fprintf(stderr, "DEBUG: Writing file %s\n", filename);
    fprintf(fpga_man, "%s\n", filename);
    fclose(fpga_man);
    sync_message_katcl(l, KATCP_LEVEL_INFO, NULL, "programming FPGA with %s", filename ? filename + 1 : pd->t_name);
    fprintf(stderr, "DEBUG: Programming FPGA with %s\n", filename ? filename + 1 : pd->t_name);
    //fprintf(fpga_man, "tcpborphserver.bin\n");
    fprintf(stderr, "DEBUG: Closed FPGA Manager\n");
  }
#endif

  alarm(0);

  return 0;
}

int transfer_status_tbs(struct katcp_dispatch *d, struct katcp_notice *n)
{
  fprintf(stderr, "DEBUG: Inside transfer_status_tbs\n");

  struct katcl_parse *px;
  char *inform, *status;

  log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "got something from job via notice %p", n);
  fprintf(stderr, "\tDEBUG: Got something from job via notice\n");

  px = get_parse_notice_katcp(d, n);
  if(px == NULL){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "no message returned by completed task");
    fprintf(stderr, "\tDEBUG: No message returned by completed task\n");
    return -1;
  }
  fprintf(stderr, "\tDEBUG: Raw KATCP response from job:\n");
  for (int i = 0; i < 5; i++) {
    char *s = get_string_parse_katcl(px, i);
    if(s){
      fprintf(stderr, "\t\t  px[%d] = '%s'\n", i, s);
    } else {
      fprintf(stderr, "\t\t  px[%d] = NULL\n", i);
    }
  }

  inform = get_string_parse_katcl(px, 0);
  fprintf(stderr, "\tDEBUG: Received inform %s\n", inform);

  if(inform == NULL){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "null parameter in status message");
    fprintf(stderr, "\tDEBUG: Null parameter in status message\n");
    return -1;
  }

  if(strcmp(inform, KATCP_RETURN_JOB)){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "expected to see a return inform, got %s instead", inform);
    return -1;
    fprintf(stderr, "\tDEBUG: Expected to see a return inform, got %s instead\n", inform);
  }

  status = get_string_parse_katcl(px, 1);
  fprintf(stderr, "\tDEBUG: inform='%s' status='%s'\n", inform ? inform : "NULL", status ? status : "NULL");
  if(status == NULL){
    log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "null status field");
    fprintf(stderr, "\tDEBUG: Null status field\n");
    return -1;
  }

  if(strcmp(status, KATCP_OK) != 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "transfer returned status %s", status);
    fprintf(stderr, "\tDEBUG: Transfer returned status %s\n", status);
    return -1;
  }

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "transfer appears to have succeeded");
  fprintf(stderr, "\tDEBUG: Transfer appears to have succceeded\n");

  return 0;
}

int upload_generic_resume_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: Inside upload_generic_resume_tbs\n");

  int result;

  result = transfer_status_tbs(d, n);

  prepend_reply_katcp(d);
  append_string_katcp(d, KATCP_FLAG_LAST, (result == 0) ? KATCP_OK : KATCP_FAIL);

  resume_katcp(d);

  return 0;
}

int sane_port_tbs(struct katcp_dispatch *d, unsigned int port)
{
  fprintf(stderr, "DEBUG: Inside sane_port_tbs\n");

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
  fprintf(stderr, "DEBUG: Inside detect_file_tbs\n");
  #define BUFFER 128
  int rfd; //, rr;
  char buffer[BUFFER];
  char bofmagic[4] = { 0x19, 'B', 'O', 'F' };

  /* TODO - use gzopen */

  /*
  if(fd < 0){
    if(name == NULL){
      fprintf(stderr, "\tDEBUG: No file given to examine\n");
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "no file given to examine");
      return TBS_FORMAT_BAD;
    }
    rfd = open(name, O_RDONLY);
    if(rfd < 0){
      fprintf(stderr, "\tDEBUG: Unable to open %s: %s\n", name, strerror(errno));
      log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "unable to open %s: %s", name, strerror(errno));
      return TBS_FORMAT_BAD;
    }
  } else {
    if(lseek(fd, 0, SEEK_SET) < 0){
      fprintf(stderr, "\tDEBUG: Unable to seek to start: %s", strerror(errno));
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "unable to seek to start: %s", strerror(errno));
      return TBS_FORMAT_BAD;
    }
    fprintf(stderr, "DEBUG: Opened file %s\n", name);
    rfd = fd;
  }
  */
  rfd = open(name, O_RDONLY);
  if (rfd < 0) {
      fprintf(stderr, "\tDEBUG: Unable to open %s: %s\n", name, strerror(errno));
      return TBS_FORMAT_BAD;
  }
  ssize_t rr = read(rfd, buffer, BUFFER - 1);
  //rr = read(rfd, buffer, BUFFER);
  fprintf(stderr, "DEBUG: Read %d bytes\n", rr);
  if (rr <= 0) {
      fprintf(stderr, "DEBUG: rr < 0 (rr = %d) bad format\n", rr);
      close(rfd);
      return TBS_FORMAT_BAD;
  }
  /*
  if(rfd != fd){
    fprintf(stderr, "DEBUG: rfd is %d while fd is %d\n", rfd, fd);
    close(rfd);
  }
  
  if(rr < BUFFER){
    fprintf(stderr, "\tDEBUG: Read failed: %s\n", (rr < 0) ? strerror(errno) : "insufficient data");
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "read failed: %s", (rr < 0) ? strerror(errno) : "insufficient data");
    return TBS_FORMAT_BAD;
  }
  */

  //buffer[BUFFER - 1] = '\0';
  buffer[rr] = '\0';  // Proper null termination
  if(!strncmp(buffer, bofmagic, 4)){
    fprintf(stderr, "\tDEBUG: Detected bof file\n");
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected bof file");
    return TBS_FORMAT_BOF;
  }

  if(buffer[0] == KATCP_REQUEST){
    fprintf(stderr, "\tDEBUG: Detected fpg file without header\n");
    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected fpg file without header");
    return TBS_FORMAT_FPG;
  }

  if(!strncmp(buffer, "#!", 2)){
    fprintf(stderr, "\tDEBUG: Detected the right shebang\n");
    if(strstr(buffer, TBS_KCPFPG_EXE)){
      fprintf(stderr, "\tDEBUG: Detected .fpg file\n");
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "detected fpg file");
      return TBS_FORMAT_FPG;
    }
  }
  fprintf(stderr, "\tDEBUG: Detected unknown file format for %s won't proceed further\n", buffer);
  fprintf(stderr, "DEBUG: buffer[0] = 0x%02x '%c'\n", buffer[0], buffer[0]);
  fprintf(stderr, "DEBUG: buffer[1] = 0x%02x '%c'\n", buffer[1], buffer[1]);
  log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "unknown file format");

  return TBS_FORMAT_BAD;
#undef BUFFER
}


/****************************************************************************/

int upload_filesystem_complete_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: Inside upload_file_system_complete_tbs\n");

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

  return 0;
}

int upload_filesystem_cmd(struct katcp_dispatch *d, int argc)
{
  fprintf(stderr, "DEBUG: Inside upload_filesystem_cmd\n");

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
  fprintf(stderr, "DEBUG: Inside upload_bin_complete_tbs\n");

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
  fprintf(stderr, "DEBUG: Inside upload_bin_cmd\n");
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
  fprintf(stderr, "DEBUG: The kurl string is %s\n", url->u_str);
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
  fprintf(stderr, "DEBUG: About to dislodge child process for programming\n");

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
  fprintf(stderr, "DEBUG: Awaiting transfer on port %d\n", pd->t_port);

  return KATCP_RESULT_PAUSE;
}

/******************************************************************************************/

int upload_program_complete_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: Inside upload_program_complete_tbs\n");

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

  return 0;
}

int upload_program_partial_tbs(struct katcp_dispatch *d, struct katcp_notice *n, void *data)
{
  fprintf(stderr, "DEBUG: Inside upload_program_partial_tbs\n");
  struct tbs_port_data *pd;
  struct bof_state *bs;
  struct katcp_job *j;
  struct katcp_notice *nx;
  struct katcp_dispatch *dl;
  int type, result;
  char *argv[3];

  pd = data;
  if(pd == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: no port data given to handler\n");
    abort();
#endif
    return -1;
  }

  if(pd->t_name == NULL){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "logic problem: null name in upload data structure\n");
    abort();
#endif
    return -1;
  }

  result = transfer_status_tbs(d, n);
  fprintf(stderr, "\tDEBUG: Result of transfer_status_tbs is %d\n", result);
  if(result < 0){
    destroy_port_data_tbs(d, pd, 1);
    return -1;
  }

  nx = find_notice_katcp(d, TBS_KCPFPG_PATH);
  if(nx){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "not proceeding with programming as another instance is already in flight");
    destroy_port_data_tbs(d, pd, 1);
    return -1;
  }

  if(stop_fpga_tbs(d) < 0){
    destroy_port_data_tbs(d, pd, 1);
    return -1;
  }

  type = detect_file_tbs(d, pd->t_name, pd->t_fd);
  fprintf(stderr, "\tDEBUG: The file type is %d\n", type);
  switch(type){
    case TBS_FORMAT_BOF :
     fprintf(stderr, "\tDEBUG: Processing upload as bof format\n");

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
      log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "assuming new fpg format for %s", pd->t_name);
      fprintf(stderr, "\tDEBUG: Assuming new fpg format for %s\n", pd->t_name);

      dl = template_shared_katcp(d);
      if(dl == NULL){
        destroy_port_data_tbs(d, pd, 1);
        return KATCP_RESULT_FAIL;
      }

      nx = create_notice_katcp(d, TBS_KCPFPG_PATH, 0);
      if(nx == NULL){
        fprintf(stderr, "\tDEBUG: Unable to create notification logic to trigger when %s completes\n", TBS_KCPFPG_PATH);
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when %s completes", TBS_KCPFPG_PATH);
        destroy_port_data_tbs(d, pd, 1);
        return -1;
      }

      if(add_notice_katcp(d, nx, &upload_program_complete_tbs, pd) < 0){
        fprintf(stderr, "\tDEBUG: Unable to register callback to resume command\n");
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback to resume command");
        destroy_port_data_tbs(d, pd, 1);
        return KATCP_RESULT_FAIL;
      }

      argv[0] = TBS_KCPFPG_PATH;
      argv[1] = pd->t_name;
      argv[2] = NULL;
      if(!argv[2]){
        fprintf(stderr, "\tDEBUG: Arguments are \n\t\t\targv[0] = %s, \n\t\t\targv[1], = %s \n\t\t\targv[2] = NULL\n", argv[0], argv[1]);
      }
      else{
        fprintf(stderr, "\tDEBUG: Arguments are \n\t\t\targv[0] = %s, \n\t\t\targv[1] = %s \n\t\t\targv[2] = %s\n", argv[0], argv[1], argv[2]);
      }
        j = process_name_create_job_katcp(dl, TBS_KCPFPG_PATH, argv, nx, NULL);
      if (j == NULL){
        fprintf(stderr, "\tDEBUG: Unable to run %s child process\n", TBS_KCPFPG_PATH);
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run %s child process", TBS_KCPFPG_PATH);
#if 1
        destroy_port_data_tbs(d, pd, 1);
#endif
        return -1;
      }

      return 0;

    default :
      fprintf(stderr, "\tDEBUG: Was sent unusable or invalid format\n");
  
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
  fprintf(stderr, "DEBUG: Inside upload_program_cmd\n");

  struct katcp_dispatch *dl;
  struct katcp_job *j;
  struct katcp_url *url;
  struct tbs_port_data *pd;
  unsigned int port, timeout, expected;
  struct tbs_raw *tr;
  struct katcp_notice *nx;
  const char *ramfile_path = TBS_RAMFILE_PATH; //is_intel_fpga() ? TBS_RAMFILE_PATH_INTEL : TBS_RAMFILE_PATH_XILINX;
  fprintf(stderr, "DEBUG: The ramfile_path is %s\n", ramfile_path);
  unsigned int delete_mode = is_intel_fpga() ? TBS_DEL_NEVER : TBS_DEL_ALWAYS;
  
  fprintf(stderr, "DEBUG: Running as UID %d\n", getuid());
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

  nx = find_notice_katcp(d, (char *)ramfile_path);
  if(nx){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "another upload already seems in progress, halting this attempt");
    return KATCP_RESULT_FAIL;
  }

  stop_fpga_tbs(d);

  nx = create_notice_katcp(d, (char *)ramfile_path, 0);
  if(nx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to create notification logic to trigger when upload completes");
    return KATCP_RESULT_FAIL;
  }

    // Set program to 0
    //pd = create_port_data_tbs(d, (char *)ramfile_path, port, 0, expected, timeout, TBS_FORMAT_ANY, delete_mode);
    pd = create_port_data_tbs(d, (char *)ramfile_path, port, 0, expected, timeout, TBS_FORMAT_ANY, TBS_DEL_NEVER);

    //pd = create_port_data_tbs(d, (char *)ramfile_path, port, 1, expected, timeout, TBS_FORMAT_ANY, delete_mode);
    //pd = create_port_data_tbs(d, TBS_RAMFILE_PATH, port, 0, expected, timeout, TBS_FORMAT_ANY, TBS_DEL_ALWAYS);

    //pd = create_port_data_tbs(d, (char *)ramfile_path, port, 1, expected, timeout, TBS_FORMAT_FPG, delete_mode);

  if (pd == NULL){
    fprintf(stderr, "DEBUG: %s couldn't create port data\n", __func__);
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: couldn't create port data", __func__);
    return KATCP_RESULT_FAIL;
  }

  /* added in the global space dl, so that it completes even if client goes away */
  if(add_notice_katcp(dl, nx, &upload_program_partial_tbs, pd) < 0){
    fprintf(stderr, "DEBUG: Unable to register callback for upload completion\n");
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to register callback for upload completion");
    return KATCP_RESULT_FAIL;
  }

  url = create_exec_kurl_katcp("progremote");
  fprintf(stderr, "DEBUG: kurl url is %s\n", url->u_str);

  if (url == NULL){
    fprintf(stderr, "DEBUG: %s could not create kurl\n", __func__);
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "%s: could not create kurl", __func__);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  j = find_job_katcp(dl, url->u_str);
  if (j){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "found job for %s", url->u_str);
    fprintf(stderr, "DEBUG: Found job for %s", url->u_str);
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
  j = run_child_process_tbs(dl, url, &subprocess_upload_tbs, pd, nx);
  //char *argv[] = { TBS_KCPFPG_PATH, pd->t_name, NULL };
  //j = process_name_create_job_katcp(dl, TBS_KCPFPG_PATH, argv, nx, NULL);
  if (j == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to run child process");
    fprintf(stderr, "DEBUG: Unable to run child process\n");

    destroy_kurl_katcp(url);
    destroy_port_data_tbs(d, pd, 1);
    return KATCP_RESULT_FAIL;
  }

  log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "awaiting transfer on port %d", pd->t_port);
  fprintf(stderr, "DEBUG: Awaiting transfer on port %d\n", pd->t_port);


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
