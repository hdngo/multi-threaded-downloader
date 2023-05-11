/*
===============================================================
                  MULTI-THREADED DOWNLOADER
                                      - Huy Ngo
===============================================================

*/

/* ===============================================================
                            INCLUDES
=============================================================== */
#include <curl/curl.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ===============================================================
                              STRUCTS
=============================================================== */
typedef struct {
  char *url;        // URL to download from
  char *filename;   // filename to save to
  int max_threads;  // maximum number of threads
} DLSettings;       // settings for downloader

typedef struct {
  int index;                 // thread index
  char *url;                 // URL to download from
  unsigned long long start;  // start byte
  unsigned long long end;    // end byte
} DLThreadArgs;              // arguments for each thread

typedef struct {
  pthread_t thread;    // thread handle
  DLThreadArgs *args;  // thread arguments
  CURL *curl;          // curl handle
  FILE *buffer;        // file handle
} DLThreadInfo;        // information about each thread

typedef struct {
  curl_off_t *total_bytes;       // total bytes to download
  curl_off_t *downloaded_bytes;  // downloaded bytes so far
} DLProgress;                    // progress information

/* ===============================================================
                          DEFS and GLOBALS
=============================================================== */
#define DEFAULT_MAX_THREADS 4
#define CLEAR_SCREEN "\033[2J\033[1;1H"
#define CHECKMARK "\u2713"
#define CROSSMARK "\u2717"
#define BOLD "\033[1m"
#define RESET "\033[0m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"
#define GREY "\033[90m"

DLThreadInfo **thread_infos;  // global array of thread_infos
DLProgress progress;          // global progress
DLSettings settings;          // global settings
int window_width;             // terminal width
int window_height;            // terminal height
char log_buffer[2048];        // buffer to store logs from threads
pthread_mutex_t completed_mutex =
    PTHREAD_MUTEX_INITIALIZER;  // mutex for completed_counter
int completed_counter = 0;      // counter for completed threads
time_t start_time;              // start time of download
bool paused;                    // whether download is paused

/* ===============================================================
                      RENDERING and INTERFACE
=============================================================== */
// Calculate width and print at center
void print_center(char *str) {
  int padding = (window_width - strlen(str)) / 2;
  printf("%*s", padding, "");
  printf("%s", str);
}

// Print header based on screen size
void print_header() {
  for (int i = 0; i < window_width; i++) {
    printf("=");
  }
  printf(RESET "\n\n" BOLD RED);
  print_center("MULTI-THREADED DOWNLOADER");
  printf("\n" RESET CYAN);
  print_center("by Huy Ngo");
  printf("\n\n" RESET);
  for (int i = 0; i < window_width; i++) {
    printf("=");
  }
  printf("\n\n" RESET);
}

// Print download info
void print_download_info() {
  printf(BOLD);
  print_center("[ Download Info ]");
  printf("\n\n" RESET CYAN BOLD);
  print_center(settings.url);
  printf("\n" GREEN);
  print_center(settings.filename);
  printf("\n\n" RESET);
}

// Clear screen
void clear_screen() { system("clear"); }

/* ===============================================================
                          DOWNLOAD SETUP
=============================================================== */
// Use getopts to parse command line arguments
void parse_args(int argc, char *argv[]) {
  // ./mtdown -u <url> -o <filename> -n <max_threads>
  int opt;
  while ((opt = getopt(argc, argv, "u:o:n:")) != -1) {
    switch (opt) {
      case 'u':
        settings.url = optarg;
        break;
      case 'o':
        settings.filename = optarg;
        break;
      case 'n':
        // Check if optarg is a number using atoi
        if (atoi(optarg) == 0) {
          fprintf(stderr, "Error: max_threads must be a number\n");
          exit(EXIT_FAILURE);
        }
        // Check if optarg is valid (1 - 32)
        if (atoi(optarg) < 1 || atoi(optarg) > 32) {
          fprintf(stderr, "Error: max_threads must be between 1 and 32\n");
          exit(EXIT_FAILURE);
        }
        settings.max_threads = atoi(optarg);
        break;
      default:
        fprintf(stderr, "Usage: %s -u <url> -o <filename> -n <max_threads>\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }
  }

  // Check if url is provided
  if (settings.url == NULL) {
    fprintf(stderr, "Usage: %s -u <url> -o <filename> -n <max_threads>\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }

  // Check if filename is provided
  if (settings.filename == NULL) {
    fprintf(stderr, "Usage: %s -u <url> -o <filename> -n <max_threads>\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }

  // Check if max_threads is provided
  if (settings.max_threads == 0) {
    settings.max_threads = DEFAULT_MAX_THREADS;
  }
}

// Callback function to disable writing from curl, this is to gather data about
// the server before actually downloading
size_t no_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  return size * nmemb;
}

// Find max concurrent connection the server allows by sending a series of
// concurrent requests and then record when a thread fails to receive data
void *find_max_thread_worker() {
  // Setup curl
  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, settings.url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, no_write_callback);
  curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)1);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
  curl_easy_perform(curl);
  int res = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res);
  curl_easy_cleanup(curl);
  // Return res as void pointer for later conversion
  return (void *)res;
}
int find_max_threads() {
  clear_screen();
  print_header();

  int max_threads = 1;

  printf("Finding maximum concurrent connections supported by server...\n");

  // Find max concurrent connections by testing the maximum number of concurrent
  // connections the server allows before returning an error response.
  for (int i = 1; i <= settings.max_threads; i++) {
    printf("Trying %d threads... ", i);
    fflush(stdout);

    pthread_t threads[i];

    for (int j = 0; j < i; j++)
      pthread_create(&threads[j], NULL, find_max_thread_worker, NULL);

    for (int j = 0; j < i; j++) {
      void *res;
      pthread_join(threads[j], &res);
      if ((int)res != 200) {
        printf(RED "%s\n" RESET, CROSSMARK);
        return i - 1;
      }
    }
    printf(GREEN "%s\n" RESET, CHECKMARK);
    max_threads = i;

    // Sleep to give the server time to recover
    sleep(1);
  }

  return max_threads;
}

// Callback function for writing to buffer
size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  // Get buffer
  FILE *buffer = (FILE *)userdata;
  size_t realsize = size * nmemb;

  // Write to buffer (the buffer is the file descriptor each thread is writing
  // to)
  fwrite(ptr, size, nmemb, buffer);

  return realsize;
}

// Progress callback for updating global progress
size_t progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t ultotal, curl_off_t ulnow) {
  // Get args from clientp
  DLThreadArgs *args = (DLThreadArgs *)clientp;

  // Update progress total bytes at index
  progress.total_bytes[args->index] = dltotal;
  progress.downloaded_bytes[args->index] = dlnow;

  return 0;
}

// Fetch content length, calculate chunk size, and setup worker threads to
// download to each part of the buffer
void *download_worker(void *info) {
  // Get thread info and args
  DLThreadInfo *thread_info = (DLThreadInfo *)info;
  DLThreadArgs *thread_args = thread_info->args;

  // Get curl
  CURL *curl = thread_info->curl;

  // Error string
  char errbuf[CURL_ERROR_SIZE];

  // Get download range
  char range[128];
  snprintf(range, sizeof(range), "%llu-%llu", thread_args->start,
           thread_args->end);

  // Set curl options
  curl_easy_setopt(curl, CURLOPT_URL, settings.url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, thread_info->buffer);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "mtdown/1.0");
  curl_easy_setopt(curl, CURLOPT_RANGE, range);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, thread_args);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

  CURLcode res;

  // Download file, if error try for another 4 times, then exit if still broken
  for (int i = 0; i < 5; i++) {
    res = curl_easy_perform(curl);

    if (res == CURLE_OK) break;

    // Add thread id and error to thread_info->logs with strcat
    char log[310];
    if (i == 4)
      snprintf(log, sizeof(log),
               RED "ERROR | Thread %d: %s, exiting...\n" RESET,
               thread_args->index, errbuf);
    else
      snprintf(log, sizeof(log),
               RED "ERROR | Thread %d: %s, retrying...\n" RESET,
               thread_args->index, errbuf);
    strcat(log_buffer, log);

    // Reset file pointer to thread_args start
    fseek(thread_info->buffer, thread_args->start, SEEK_SET);

    sleep(1);
  }

  // Cleanup curl
  curl_easy_cleanup(curl);

  // Increase completed counter
  pthread_mutex_lock(&completed_mutex);
  completed_counter++;
  pthread_mutex_unlock(&completed_mutex);

  // Close buffer
  fclose(thread_info->buffer);

  return NULL;
}
void setup_download() {
  // Fetch content length
  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, settings.url);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_perform(curl);
  curl_off_t res = 0;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &res);
  curl_easy_cleanup(curl);

  // Check if content length is valid
  if (res <= 0) {
    printf("ERROR | Could not fetch content length\n");
    exit(EXIT_FAILURE);
  }

  // Calculate chunk size
  curl_off_t chunk_size = res / settings.max_threads;

  // Setup global thread_info array
  thread_infos = malloc(sizeof(DLThreadInfo *) * settings.max_threads);

  // Check error
  if (thread_infos == NULL) {
    printf("ERROR | Could not allocate thread_infos\n");
    exit(EXIT_FAILURE);
  }

  // Malloc total bytes in progress
  progress.downloaded_bytes = malloc(sizeof(curl_off_t) * settings.max_threads);
  progress.total_bytes = malloc(sizeof(curl_off_t) * settings.max_threads);

  // Check error
  if (progress.downloaded_bytes == NULL || progress.total_bytes == NULL) {
    printf("ERROR | Could not allocate progress\n");
    exit(EXIT_FAILURE);
  }

  // Set paused to false
  paused = false;

  // Check if file exists, asks user if they want to overwrite
  FILE *file = fopen(settings.filename, "r");
  if (file != NULL) {
    fclose(file);
    printf(RED BOLD "\nFile %s already exists, overwrite? (y/n) " RESET,
           settings.filename);
    char c;
    scanf("%c", &c);
    if (c == 'n') exit(EXIT_SUCCESS);
  }

  // Create file of size res for all threads to write into
  file = fopen(settings.filename, "wb");

  // Check error
  if (file == NULL) {
    printf("ERROR | Could not create file %s\n", settings.filename);
    exit(EXIT_FAILURE);
  }

  // Allocate size of res
  fallocate(fileno(file), 0, 0, res);
  if (ferror(file)) {
    printf("ERROR | Could not allocate file %s\n", settings.filename);
    exit(EXIT_FAILURE);
  }

  fclose(file);

  // Setup worker threads using global threads array, each buffer is
  // thread-specific
  for (int i = 0; i < settings.max_threads; i++) {
    // Allocate spot in thread_info array
    thread_infos[i] = malloc(sizeof(DLThreadInfo));

    // Check error
    if (thread_infos[i] == NULL) {
      printf("ERROR | Could not allocate thread_info for thread %d\n", i);
      exit(EXIT_FAILURE);
    }

    // Allocate a file buffer for each thread
    // Seek accordingly
    thread_infos[i]->buffer = fopen(settings.filename, "wb");

    // Check error
    if (thread_infos[i]->buffer == NULL) {
      printf("ERROR | Could not open file %s for thread %d\n",
             settings.filename, i);
      exit(EXIT_FAILURE);
    }

    fseek(thread_infos[i]->buffer, i * chunk_size, SEEK_SET);

    // Allocate args
    thread_infos[i]->args = malloc(sizeof(DLThreadArgs));

    // Check error
    if (thread_infos[i]->args == NULL) {
      printf("ERROR | Could not allocate thread_args for thread %d\n", i);
      exit(EXIT_FAILURE);
    }

    thread_infos[i]->args->index = i;
    thread_infos[i]->args->start = i * chunk_size;
    thread_infos[i]->args->end = (i + 1) * chunk_size - 1;

    // Set end of last thread to content length
    if (i == settings.max_threads - 1) thread_infos[i]->args->end = res;

    // Assign the rest of the thread info
    thread_infos[i]->curl = curl_easy_init();

    // Add download started to log
    char log[256];
    snprintf(log, sizeof(log),
             GREY " INFO | Thread %d started downloading.\n" RESET, i);
    strcat(log_buffer, log);

    // Create thread
    pthread_create(&thread_infos[i]->thread, NULL, download_worker,
                   thread_infos[i]);
  }
}

/* ===============================================================
                      PROGRESS and POST-DOWNLOAD
=============================================================== */
// Print bytes downloaded/total bytes and progress
void printProgress(curl_off_t downloaded, curl_off_t total) {
  // Find suitable unit for downloaded and total
  if (total > 1000000000)
    printf("%.2f / %.2f GB (%.2f%%)\n", (double)downloaded / 1000000000,
           (double)total / 1000000000, (double)downloaded / total * 100);
  else if (total > 1000000)
    printf("%.2f / %.2f MB (%.2f%%)\n", (double)downloaded / 1000000,
           (double)total / 1000000, (double)downloaded / total * 100);
  else if (total > 1000)
    printf("%.2f / %.2f KB (%.2f%%)\n", (double)downloaded / 1000,
           (double)total / 1000, (double)downloaded / total * 100);
  else
    printf("%ld / %ld B (%.2f%%)\n", downloaded, total,
           (double)downloaded / total * 100);
}

// Print speed and ETA
void printSpeed(curl_off_t downloaded, curl_off_t total, time_t start_time,
                time_t elapsed_time) {
  // Calculate speed and eta
  double speed = (double)downloaded / (time(NULL) - start_time);
  double eta = (double)(total - downloaded) / speed;

  // Find suitable unit for speed
  if (speed > 1000000000)
    printf("%.2f GB/s", speed / 1000000000);
  else if (speed > 1000000)
    printf("%.2f MB/s", speed / 1000000);
  else if (speed > 1000)
    printf("%.2f KB/s", speed / 1000);
  else
    printf("%.2f B/s", speed);

  // Find suitable unit for ETA
  if (eta > 3600)
    printf(" (%.2f hours remaining)\n", eta / 3600);
  else if (eta > 60)
    printf(" (%.2f minutes remaining)\n", eta / 60);
  else
    printf(" (%.2f seconds remaining)\n", eta);
}

// Pause handler
void pause_handler() {
  if (paused) {
    // Resume all threads
    for (int i = 0; i < settings.max_threads; i++)
      curl_easy_pause(thread_infos[i]->curl, CURLPAUSE_CONT);
    paused = false;

    // Print to log
    char log[256];
    snprintf(log, sizeof(log), GREEN " INFO | Download resumed.\n" RESET);
    strcat(log_buffer, log);
  } else {
    // Pause all threads
    for (int i = 0; i < settings.max_threads; i++)
      curl_easy_pause(thread_infos[i]->curl, CURLPAUSE_RECV);
    paused = true;
    // Print to log
    char log[256];
    snprintf(log, sizeof(log), YELLOW " INFO | Download paused.\n" RESET);
    strcat(log_buffer, log);
  }
}

// Quit handler
void quit_handler() {
  // Print to log
  char log[256];
  snprintf(log, sizeof(log),
           RED "ERROR | Download cancelled by user, exiting...\n" RESET);
  strcat(log_buffer, log);
}

// Wait for all threads to complete, print status and progress bar
void wait_for_threads() {
  while (completed_counter < settings.max_threads) {
    // ncurses used here for non blocking read, allowing pause and quit at
    // anytime
    initscr();
    // Get new window size in case of resize
    getmaxyx(stdscr, window_height, window_width);
    timeout(500);
    noecho();
    cbreak();
    char c = getch();
    if (c == 'p' || c == 'P') {
      pause_handler();
    }
    if (c == 'q' || c == 'Q') {
      quit_handler();
    }
    endwin();

    // Start printing progress
    clear_screen();
    print_header();
    print_download_info();

    // Progress Bar and Status
    double thread_bar_length = window_width - 45;
    curl_off_t total_downloaded = 0;
    curl_off_t total_bytes = 0;

    printf(BOLD);
    print_center("[ Progress | Press P to pause, Q to quit ]");
    printf("\n\n" RESET);

    for (int i = 0; i < settings.max_threads; i++) {
      total_downloaded += progress.downloaded_bytes[i];
      total_bytes += progress.total_bytes[i];

      printf(" Thread %d: " WHITE, i);

      for (double j = 0; j < (double)progress.downloaded_bytes[i] /
                                 progress.total_bytes[i] * thread_bar_length;
           j++) {
        printf("█");
      }

      printf(GREY);

      for (double j = (double)progress.downloaded_bytes[i] /
                      progress.total_bytes[i] * thread_bar_length;
           j < thread_bar_length; j++) {
        printf("█");
      }

      printf(" " RESET);
      printProgress(progress.downloaded_bytes[i], progress.total_bytes[i]);
    }

    // Update speed and progress
    time_t current_time = time(NULL);
    double elapsed_time = difftime(current_time, start_time);
    double speed = total_downloaded / elapsed_time;

    // Print progress
    printf("\n");
    for (int i = 0; i < (window_width - 24) / 2; i++) printf(" ");
    printProgress(total_downloaded, total_bytes);

    // Print speed and ETA
    for (int i = 0; i < (window_width - 37) / 2; i++) printf(" ");
    printSpeed(total_downloaded, total_bytes, start_time, elapsed_time);

    // Check for logs
    printf("\n" BOLD);
    print_center("[ Logs ]");
    printf("\n" RESET);
    printf("%s", log_buffer);

    // Exit if "exiting..." is found in logs
    if (strstr(log_buffer, "exiting...") != NULL) {
      for (int i = 0; i < settings.max_threads; i++) {
        curl_easy_setopt(thread_infos[i]->curl, CURLOPT_TIMEOUT_MS, 1);
        pthread_cancel(thread_infos[i]->thread);
      }
      return;
    }
  }
}

// Free everything if exist
void free_all() {
  // Free thread info
  for (int i = 0; i < settings.max_threads; i++) {
    if (thread_infos[i] && thread_infos[i]->args) free(thread_infos[i]->args);
    if (thread_infos[i]) free(thread_infos[i]);
  }
  if (thread_infos) free(thread_infos);

  // Free progress
  if (progress.downloaded_bytes) free(progress.downloaded_bytes);
  if (progress.total_bytes) free(progress.total_bytes);
}

/* ===============================================================
                              MAIN
=============================================================== */
int main(int argc, char *argv[]) {
  // Get window width and height
  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
  window_width = w.ws_col;
  window_height = w.ws_row;

  // Parse command line arguments
  parse_args(argc, argv);

  // Initialize curl
  curl_global_init(CURL_GLOBAL_ALL);

  // Find max concurrent connection the server allows
  settings.max_threads = find_max_threads();
  printf(BOLD "\nMax threads updated: %d\n" RESET
              "Starting download in 2 seconds...\n",
         settings.max_threads);
  sleep(1);

  // Init global mutex
  pthread_mutex_init(&completed_mutex, NULL);

  // Setup download
  setup_download();

  // Start timer
  start_time = time(NULL);

  // Wait for all threads to complete
  wait_for_threads();

  // Print finish
  printf("\n\n" GREEN BOLD);
  print_center("Download Complete ");
  printf(CHECKMARK "\n" RESET);

  // Destroy mutex
  pthread_mutex_destroy(&completed_mutex);

  // Free everything
  free_all();

  // Cleanup curl
  curl_global_cleanup();

  return 0;
}
