#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 1024
#define N_INTERVALS 128

int sock_fd;

FILE *f;
long cur_f_size = 0;

struct sockaddr_in addr;

struct missing_block_t {
  int block;
  struct missing_block_t *next;
};
struct missing_block_t *missing_blocks = NULL;
int max_block_seen = -1;
int total_blocks = -1;

int last_announce = 0;
int last_log = 0;

struct missing_interval_t {
  int first;
  int last;
};
struct missing_report_t {
  char magic[4];
  struct missing_interval_t intervals[N_INTERVALS];
};

void maybe_announce() {
  if (last_log < time(NULL)) {
    last_log = time(NULL);

    int n_missing = 0;
    for (struct missing_block_t *it = missing_blocks; it; it = it->next) {
      n_missing++;
    }

    fprintf(stderr,
            "%d blocks missing\nmax block seen: %d\ntotal blocks: %d\n\n",
            n_missing, max_block_seen, total_blocks);
    fflush(stderr);
  }

  if (missing_blocks == NULL && max_block_seen == total_blocks - 1) {
    fprintf(stderr, "done\n");
    exit(0);
  }

  if ((missing_blocks != NULL || max_block_seen < total_blocks - 1) &&
      last_announce < time(NULL) - 5) {
    last_announce = time(NULL);

    struct missing_report_t buf;
    memcpy(&buf.magic, "REQT", 4);

    int i = 0;
    for (struct missing_block_t *it = missing_blocks; it; it = it->next) {
      if (i > 0 && it->block == buf.intervals[i - 1].last + 1) {
        buf.intervals[i - 1].last++;
      } else {
        if (i == N_INTERVALS) {
          break;
        }
        buf.intervals[i].first = it->block;
        buf.intervals[i].last = it->block;
        i++;
      }
    }

    if (i < N_INTERVALS && max_block_seen < total_blocks - 1) {
      buf.intervals[i].first = max_block_seen + 1;
      buf.intervals[i].last = total_blocks - 1;
      i++;
    }

    for (int j = 0; j < i; j++) {
      buf.intervals[j].first = htonl(buf.intervals[j].first);
      buf.intervals[j].last = htonl(buf.intervals[j].last);
    }

    if (sendto(sock_fd, &buf, 4 + i * sizeof(struct missing_interval_t), 0,
               (struct sockaddr *)&addr, sizeof(addr)) == -1) {
      perror("sendto");
      exit(1);
    }
  }
}

void receive_pending() {
  for (;;) {
    char buf_to_recv[12 + BLOCK_SIZE];
    int n_recv = recv(sock_fd, buf_to_recv, sizeof(buf_to_recv), MSG_DONTWAIT);
    if (n_recv == -1) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return;
      }
      perror("recv");
      exit(1);
    }

    if (n_recv >= 8 && memcmp(buf_to_recv, "SHRE", 4) == 0) {
      int block = ntohl(*(int *)(buf_to_recv + 4));

      if (block > max_block_seen) {
        if (block != max_block_seen + 1) {
          fprintf(stderr, "%d..%d\n", max_block_seen, block);
        }
        for (int i = max_block_seen + 1; i <= block; i++) {
          struct missing_block_t *info = malloc(sizeof(struct missing_block_t));
          info->block = i;
          info->next = missing_blocks;
          missing_blocks = info;
        }
        max_block_seen = block;
      }

      for (struct missing_block_t **it = &missing_blocks; *it;
           it = &(*it)->next) {
        if ((*it)->block == block) {
          struct missing_block_t *old_block = *it;
          *it = old_block->next;
          free(old_block);

          total_blocks = ntohl(*(int *)(buf_to_recv + 8));

          int block_size = n_recv - 12;
          long block_start = (long)block * BLOCK_SIZE;
          long block_end = block_start + block_size;
          if (cur_f_size < block_end && ftruncate(fileno(f), block_end) == -1) {
            perror("ftruncate");
            exit(1);
          }
          if (fseek(f, block_start, SEEK_SET) == -1) {
            perror("fseek");
            exit(1);
          }
          errno = 0;
          if (fwrite(buf_to_recv + 12, 1, block_size, f) < block_size) {
            perror("fwrite");
            exit(1);
          }

          break;
        }
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <file>\n", argv[0]);
    return 0;
  }

  f = fopen(argv[1], "w");
  if (!f) {
    perror("fopen");
    return 1;
  }

  sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_fd == -1) {
    perror("socket");
    return 1;
  }

  int one = 1;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
    perror("setsockopt");
    return 1;
  }

  addr.sin_family = AF_INET;
  addr.sin_port = htons(7854);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind");
    return 1;
  }
  addr.sin_addr.s_addr = htonl(0xc0a801ff) /*INADDR_BROADCAST*/;

  if (setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) == -1) {
    perror("setsockopt");
    return 1;
  }

  for (;;) {
    maybe_announce();
    receive_pending();
  }
}
