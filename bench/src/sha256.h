// SHA-256 of a file, for identifying the binary a result came from.
#ifndef CPCPUB_SHA256_H
#define CPCPUB_SHA256_H

// Hashes `path` and writes 64 lowercase hex digits plus a NUL into `out_hex`.
// Returns 0 on success, -1 if the file could not be read (out_hex is then an
// empty string). No allocation, no state kept between calls.
int sha256_file(const char *path, char out_hex[65]);

#endif
