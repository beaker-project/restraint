#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib-unix.h>

#define USOCKET_PATH "/tmp/rstrntsync.sock"
#define PORT 6776
#define BUFSIZE 256

struct sdata {
  GHashTable *events;
  GSList *wlist;
};

struct wentry {
  char *event;
  int sockfd;
};

static void wentry_free(struct wentry *w)
{
  g_free(w->event);
  close(w->sockfd);
  g_free(w);
}

static void usage(char *ename)
{
  g_print("usage:\n"
          "\t%s set <event>\n"
          "\t%s block <event> <host> [timeout]\n",
          ename, ename);
}

static gboolean quit_loop(gpointer data)
{
  GMainLoop *loop = (GMainLoop*)data;
  g_main_loop_quit(loop);
  return FALSE;
}

static gboolean on_term_signal(gpointer data)
{
  g_idle_add_full(G_PRIORITY_LOW, quit_loop, data, NULL);
  return FALSE;
}

/*
 * handle_rconn
 * ------------
 * Handles receipt of 'block' operations.
 * If a set operation was previous called, respond back to client.
 * If set operation not previously called, put this client
 *   on a pending list so we can respond back when the 'set'
 *   comes in.
 * Before we do any of this checking, make sure there are no
 * stale 'blocked clients' sockets since these can timeout
 * and go away.
 */
static gboolean handle_rconn(GIOChannel *source, GIOCondition condition,
                             gpointer data)
{
  struct sdata *sd = (struct sdata*)data;
  int sockfd = g_io_channel_unix_get_fd(source);
  ssize_t rcv = 0;
  ssize_t bytes_sent = 0;
  char buf[BUFSIZE];
  int rsock;
  struct sockaddr addr;
  unsigned int len = sizeof(addr);
  GSList *rlist = NULL;

  if ((rsock = accept(sockfd, &addr, &len)) < 0) {
    perror("Failed to accept connection");
    return TRUE;
  }

  rcv = recv(rsock, buf, BUFSIZE, 0);
  if (rcv < 0) {
    perror("Failed to receive");
    close(rsock);
    return TRUE;
  }

  /* Verify current sockets are still active */
  for (GSList *l = sd->wlist; l; l = g_slist_next(l)) {
    struct wentry *w = (struct wentry*)l->data;
    bytes_sent = send(w->sockfd, "PING", 5, MSG_NOSIGNAL);
    if (bytes_sent == -1) {
        rlist = g_slist_prepend(rlist, w);
    }
  }
  for (GSList *l = rlist; l; l = g_slist_next(l)) {
    sd->wlist = g_slist_remove(sd->wlist, l->data);
    wentry_free((struct wentry*)l->data);
  }
  g_slist_free(rlist);

  /* Now check for match */
  if (g_hash_table_contains(sd->events, buf)) {
    send(rsock, buf, rcv, MSG_NOSIGNAL);
    close(rsock);
  } else {
    struct wentry *w = malloc(sizeof(struct wentry));
    w->event = g_strdup(buf);
    w->sockfd = rsock;
    sd->wlist = g_slist_prepend(sd->wlist, w);
  }

  return TRUE;
}

/*
 * handle_lconn
 * ------------
 *
 * This handles receipt of 'set' operations.
 * This stores the 'set state' which is transmitted by the client.
 * Afterwards, it checks to determine if there are clients
 * 'blocked' waiting for this 'state' to be 'set'.
 * If there are, we acknowledge back to those clients and
 * remove it from the 'block pending' list.
 */
static gboolean handle_lconn(GIOChannel *source, GIOCondition condition,
                             gpointer data)
{
  struct sdata *sd = (struct sdata*)data;
  int sockfd = g_io_channel_unix_get_fd(source);
  int rsock;
  struct sockaddr addr;
  unsigned int len = sizeof(addr);
  ssize_t rcv = 0;
  char buf[BUFSIZE];

  if ((rsock = accept(sockfd, &addr, &len)) < 0) {
    perror("Failed to accept connection");
    return TRUE;
  }

  rcv = recv(rsock, buf, BUFSIZE, 0);
  if (rcv < 0) {
    perror("Failed to receive");
    close(rsock);
    return TRUE;
  }

  g_hash_table_add(sd->events, g_strdup(buf));

  close(rsock);

  GSList *rlist = NULL;
  for (GSList *l = sd->wlist; l; l = g_slist_next(l)) {
    struct wentry *w = (struct wentry*)l->data;
    if (g_strcmp0(w->event, buf) == 0) {
      send(w->sockfd, buf, rcv, MSG_NOSIGNAL);
      rlist = g_slist_prepend(rlist, w);
    }
  }

  for (GSList *l = rlist; l; l = g_slist_next(l)) {
    sd->wlist = g_slist_remove(sd->wlist, l->data);
    wentry_free((struct wentry*)l->data);
  }
  g_slist_free(rlist);

  return TRUE;
}

static void setevent(int sockfd, char *event)
{
  send(sockfd, event, strlen(event) + 1, 0);
}

static int listen_local(void)
{
  int sockfd;
  struct sockaddr_un saddr;

  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("Failed to open local socket");
    return -1;
  }

  saddr.sun_family = AF_UNIX;
  strcpy(saddr.sun_path, USOCKET_PATH);

  if (bind(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    perror("Failed to bind local socket");
    close(sockfd);
    return -1;
  }

  if (listen(sockfd, 5)) {
    perror("Failed to listen to local socket");
    close(sockfd);
    return -1;
  }
  return sockfd;
}
static int listen_remote(void)
{
  int sockfd;
  struct sockaddr_in saddr;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("Failed to open remote socket");
    return -1;
  }

  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(PORT);
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
    perror("Failed to bind remote socket");
    close(sockfd);
    return -1;
  }

  if (listen(sockfd, 5)) {
    perror("Failed to listen to remote socket");
    close(sockfd);
    return -1;
  }
  return sockfd;
}


/*
 * handler()
 *
 * This is the rstrnt-sync server which is spawned on first call
 * to rstrnt-sync.  It is responsible for saving 'set state'
 * and storing client socket info of those clients waiting
 * for 'set state' operation coming in.
 */
void handler(void)
{
  g_autofree struct sdata *sd = g_new0(struct sdata, 1);
  sd->events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                     g_free, NULL);
  int lsock, rsock;

  signal(SIGPIPE, SIG_IGN);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  close(STDIN_FILENO);
  if ((lsock = listen_local()) < 0) {
    goto lerror;
  }

  if ((rsock = listen_remote()) < 0) {
    goto rerror;
  }

  GMainLoop *mloop = g_main_loop_new(NULL, FALSE);
  GIOChannel *lchan = g_io_channel_unix_new(lsock);
  GIOChannel *rchan = g_io_channel_unix_new(rsock);

  g_io_add_watch(lchan, G_IO_IN, handle_lconn, sd);
  g_io_add_watch(rchan, G_IO_IN, handle_rconn, sd);
  g_unix_signal_add(SIGINT, on_term_signal, mloop);
  g_unix_signal_add(SIGTERM, on_term_signal, mloop);

  g_main_loop_run(mloop);

  close(rsock);
rerror:
  close(lsock);
lerror:
  g_hash_table_destroy(sd->events);
  g_slist_free_full(sd->wlist, (GDestroyNotify)wentry_free);
  unlink(USOCKET_PATH);
  return;
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  if (g_strcmp0(argv[1], "set") == 0) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un saddr;

    if (sockfd < 0) {
      perror("Failed to open local socket");
      return 1;
    }

    saddr.sun_family = AF_UNIX;
    strcpy(saddr.sun_path, USOCKET_PATH);

    if (connect(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
      pid_t pid;

      pid = fork();
      if (pid == 0) {
        handler();
      } else {
        guint64 i = 0;
        struct stat stbuf;
        while(stat(USOCKET_PATH, &stbuf) < 0 && errno == ENOENT) {
          g_usleep(50000);
          i++;
          if (i % 1200 == 0) {
            g_print("rhts-sync: still trying to connect to local rhts-sync daemon\n");
          }
        }
        if (connect(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
          perror("Failed to connect");
          close(sockfd);
          return 1;
        }
        setevent(sockfd, argv[2]);
      }
    } else {
      setevent(sockfd, argv[2]);
    }
    close(sockfd);
  } else if (g_strcmp0(argv[1], "block") == 0) {
    char buf[BUFSIZE] = "";
    struct sockaddr_in saddr;
    struct hostent *he;
    int sockfd, ret=0, result, bytes=0, time_rcvd=0;
    fd_set rset;
    struct timeval timeout = { 0, 0 };
    time_t start_time, end_time;
    double seconds, diff_time=0;
    unsigned int ping_count = 0;
    unsigned int non_match_count = 0;

    if (argc < 4) {
      usage(argv[0]);
      return 1;
    }

    if (argc == 5) {
      time_rcvd = atoi(argv[4]);
      timeout.tv_sec = time_rcvd;
      time(&start_time);
      time(&end_time);
      diff_time = time_rcvd;
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      perror("Failed to open local socket");
      return 1;
    }

    he = gethostbyname(argv[3]);
    if (he == NULL) {
      g_fprintf(stderr, "Failed to resolve hostname '%s'.\n",
               argv[3]);
      return 1;
    }
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(PORT);
    memcpy(&(saddr.sin_addr.s_addr), he->h_addr_list[0], he->h_length);

    if (connect(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
      g_fprintf(stderr, "Failed to connect to remote server %s: %s\n",
                argv[3], strerror(errno));
      close(sockfd);
      return 1;
    }

    send(sockfd, argv[2], strlen(argv[2]) + 1, 0);

    result = 1;
    FD_ZERO(&rset);
    FD_SET(sockfd, &rset);
    while ((time_rcvd == 0) || (diff_time > 0)) {
        if (timeout.tv_sec > 0) {
          ret = select(sockfd + 1, &rset, NULL, NULL, &timeout);
          if (ret <= 0) {
              // Error or peer is gone or nothing to read.
              break;
          }
        }
        bytes = recv(sockfd, buf, BUFSIZE, 0);
        if (bytes <= 0) {
            // Error or peer disconnected
            break;
        }

        // if server not testing the socket with PING
        // and we match the state, quit with success.
        if ((g_strcmp0("PING", buf) != 0)  &&
             (g_strcmp0(argv[2], buf) == 0)) {
            result = 0;
            break;
        }
        if (g_strcmp0("PING", buf) == 0) {
            ping_count++;
        } else {
            non_match_count++;
        }
        if (time_rcvd != 0) {
          time(&end_time);
          seconds = difftime(end_time, start_time);
          diff_time = (double)time_rcvd - seconds;
          timeout.tv_sec = diff_time;
          timeout.tv_usec = 0;
        }
    }
    close(sockfd);
    if (result == 1) {
          g_fprintf(stderr, "Server %s not reported state %s for Multihost "
                            "Sync rcvd %d bytes, ret %d [%d, %d]\n",
                    argv[3], argv[2], bytes, ret, ping_count, non_match_count);
     }
    return(result);

  }

  return 0;
}
