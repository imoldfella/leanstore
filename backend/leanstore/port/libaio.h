#define O_DIRECT 00040000
int posix_fallocate(int fd, off_t offset, off_t len);

#define BLKGETSIZE64 4*1024
