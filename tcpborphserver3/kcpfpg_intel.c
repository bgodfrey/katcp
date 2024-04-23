#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <zlib.h>

#define MTU 4096

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input.fpg>\n", argv[0]);
        return 1;
    }

    const char *input_fpg = argv[1];
    struct stat st;

    if (stat(input_fpg, &st) != 0) {
        perror("stat on input .fpg failed");
        return 1;
    }

    fprintf(stderr, "DEBUG KCPFPG_INTEL: launching on: %s\n", input_fpg);
    fprintf(stderr, "DEBUG KCPFPG_INTEL: Input .fpg size: %ld bytes\n", st.st_size);

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
        fprintf(stderr, "DEBUG KCPFPG_INTEL: Final .rbf size: %ld bytes\n", rbfs.st_size);
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