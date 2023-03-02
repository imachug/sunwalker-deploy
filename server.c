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
int total_blocks;
int last_block_sent;

FILE *f;

struct sockaddr_in addr;

struct missing_block_t {
  int block;
  int last_sent;
  int n_reports;
  struct missing_block_t *next;
};
struct missing_block_t *blocks_reported_missing = NULL;

int last_cleanup = 0;

struct missing_interval_t {
  int first;
  int last;
};
struct missing_report_t {
  char magic[4];
  struct missing_interval_t intervals[N_INTERVALS];
};

void answer_queries() {
  long cur_offset = ftell(f);
  if (cur_offset == -1) {
    perror("ftell");
    exit(1);
  }

  if (last_cleanup < time(NULL)) {
    last_cleanup = time(NULL);

    struct missing_block_t **head = &blocks_reported_missing;
    int min_reported_block = -1;
    int min_reported_block_reports = 0;
    int total_missing_blocks = 0;
    while (*head) {
      if ((*head)->last_sent < time(NULL) - 10) {
        struct missing_block_t *removed_block = *head;
        *head = removed_block->next;
        free(removed_block);
      } else {
        int block = (*head)->block;
        if (min_reported_block == -1 || block < min_reported_block) {
          min_reported_block = block;
          min_reported_block_reports = (*head)->n_reports;
        }
        total_missing_blocks++;
        head = &(*head)->next;
      }
    }

    fprintf(stderr,
            "Min recently reported block: %d (%d reports)\nTotal missing "
            "blocks: %d\nCurrent file location: %ld MiB\n\n",
            min_reported_block, min_reported_block_reports,
            total_missing_blocks, cur_offset / 1024 / 1024);
    fflush(stderr);
  }

  for (;;) {
    char buf[4 + BLOCK_SIZE];
    int n_recv = recv(sock_fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n_recv == -1) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return;
      }
      perror("recv");
      exit(1);
    }

    if (n_recv >= 4 && memcmp(buf, "REQT", 4) == 0) {
      struct missing_report_t *report = (struct missing_report_t *)buf;

      for (int i = 0; i < (n_recv - 4) / sizeof(struct missing_interval_t);
           i++) {
        int first = ntohl(report->intervals[i].first);
        int last = ntohl(report->intervals[i].last);
        printf("%d %d\n", first, last);
        for (int block = first; block <= last && block <= last_block_sent;
             block++) {
          int should_send = 1;
          int should_add = 1;
          for (struct missing_block_t *it = blocks_reported_missing; it;
               it = it->next) {
            if (it->block == block) {
              if (it->last_sent > time(NULL) - 5) {
                should_send = 0;
              } else {
                it->last_sent = time(NULL);
              }
              it->n_reports++;
              should_add = 0;
              break;
            }
          }
          if (should_add) {
            struct missing_block_t *info =
                malloc(sizeof(struct missing_block_t));
            if (!info) {
              perror("malloc");
              exit(1);
            }
            info->block = block;
            info->last_sent = time(NULL);
            info->n_reports = 1;
            info->next = blocks_reported_missing;
            blocks_reported_missing = info;
          }

          if (should_send) {
            if (fseek(f, block * BLOCK_SIZE, SEEK_SET) != -1) {
              char buf_to_send[12 + BLOCK_SIZE];
              memcpy(buf_to_send, "SHRE", 4);
              *(int *)(buf_to_send + 4) = htonl(block);
              int n_read = fread(buf_to_send + 12, 1, BLOCK_SIZE, f);
              if (n_read < BLOCK_SIZE && !feof(f)) {
                perror("fread");
                exit(1);
              }
              *(int *)(buf_to_send + 8) = htonl(total_blocks);
              if (sendto(sock_fd, buf_to_send, n_read + 12, 0,
                         (struct sockaddr *)&addr, sizeof(addr)) == -1) {
                perror("sendto");
                exit(1);
              }
            }

            if (fseek(f, cur_offset, SEEK_SET) == -1) {
              perror("fseek");
              exit(1);
            }
          }
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

  f = fopen(argv[1], "r");
  if (!f) {
    perror("fopen");
    return 1;
  }

  if (fseek(f, 0, SEEK_END) == -1) {
    perror("fseek");
    return 1;
  }
  long total_size = ftell(f);
  if (total_size == -1) {
    perror("ftell");
    return 1;
  }
  total_blocks = (total_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

  if (fseek(f, 0, SEEK_SET) == -1) {
    perror("fseek");
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
  addr.sin_addr.s_addr = 0xffffffc0 /*INADDR_BROADCAST*/;

  if (setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) == -1) {
    perror("setsockopt");
    return 1;
  }

  while (!feof(f)) {
    answer_queries();

    int block = ftell(f) / BLOCK_SIZE;
    last_block_sent = block;

    char buf_to_send[12 + BLOCK_SIZE];
    memcpy(buf_to_send, "SHRE", 4);
    *(int *)(buf_to_send + 4) = htonl(block);
    int n_read = fread(buf_to_send + 12, 1, BLOCK_SIZE, f);
    if (n_read < BLOCK_SIZE && !feof(f)) {
      perror("fread");
      exit(1);
    }
    *(int *)(buf_to_send + 8) = htonl(total_blocks);
    if (sendto(sock_fd, buf_to_send, n_read + 12, 0, (struct sockaddr *)&addr,
               sizeof(addr)) == -1) {
      perror("sendto");
      exit(1);
    }

    usleep(100);
  }

  for (;;) {
    answer_queries();
    usleep(100000);
  }
}
