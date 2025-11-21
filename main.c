#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>        // For threading (pthread_create, pthread_join)
#include <unistd.h>         // For system calls (read, close, access, usleep)
#include <signal.h>         // For signal handling (Ctrl+C)
#include <dirent.h>         // For directory listing (opendir, readdir)
#include <sys/stat.h>       // For file/directory info (stat)
#include <sys/inotify.h>    // For file watching (inotify_init, inotify_add_watch)
#include <linux/limits.h>   // For path size limits (PATH_MAX)
#include <time.h>           // For seeding random numbers (srand, time)
#include <locale.h>         // For setting the character encoding (setlocale)
#include <errno.h>          // For error codes (errno)

// ANSI escape codes for coloring text in the terminal
#define COLOR_RESET   "\x1b[0m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_LIGHT_BLUE "\x1b[94m"

// --- Globals ---

// A dynamic array structure to hold a list of strings
typedef struct {
    char** items;
    int count;
    int capacity;
} StringList;

// File to store directories
#define DIRS_FILE "dirs.txt"

// Tracks if the random number generator has been seeded
static int g_RngSeeded = 0;

// Global flag to signal all threads to stop (e.g., on Ctrl+C)
// 'volatile sig_atomic_t' is a type guaranteed to be safe in a signal handler.
static volatile sig_atomic_t g_running = 1;

// Stores the full path of the last file shown to the user
static char g_LastShownFile[PATH_MAX] = {0};

// --- Prototypes ---
StringList LoadDirs();
void SaveDirs(const StringList* dirs);
StringList GetAllFiles(const StringList* dirs);
void WriteColor(const char* color, const char* message);
void HandleOpenCommand();
void* WatcherThread(void* arg);
void SignalHandler(int signum);
void FreeStringList(StringList* list);
void AddStringToList(StringList* list, const char* str);

// ====================================================================
// PROGRAM ENTRY POINT
// ====================================================================
int main(void) {
    // Use the system's native locale (e.g., UTF-8) for all I/O.
    // This allows printing and reading special characters like Cyrillic.
    setlocale(LC_ALL, "");

    // Seed the random number generator once
    srand((unsigned int)time(NULL));
    g_RngSeeded = 1;

    // Set up a handler for Ctrl+C (SIGINT signal)
    signal(SIGINT, SignalHandler);

    WriteColor(COLOR_CYAN, "Random Filepath Displayer by Calc++\n");
    printf("Press Enter to display a random file.\n");
    printf("Type 'newdir', 'removedir', 'viewdir', 'open', or 'exit' to quit.\n\n");

    StringList dirs = LoadDirs();

    if (dirs.count == 0) {
        WriteColor(COLOR_RED, "[!!!] I have no idea where to look! Be my guest, give me a clue!\n");
    }

    // Start the background thread that watches for file changes
    pthread_t watcherThreadID;
    if (pthread_create(&watcherThreadID, NULL, WatcherThread, &dirs) != 0) {
        perror(COLOR_RED "Failed to create watcher thread" COLOR_RESET);
    }

    char cmd[PATH_MAX]; // Input buffer

    // --- Main Interactive Loop ---
    while (g_running) {
        printf(">>> ");
        fflush(stdout); // Ensure ">>>" prints before blocking on input

        // Read a line of input from the user
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
            if (g_running) printf("\n");
            g_running = 0; // Signal exit on EOF (Ctrl+D)
            break;
        }

        // Trim newline character
        cmd[strcspn(cmd, "\r\n")] = '\0';

        // Check again in case Ctrl+C was pressed while waiting for input
        if (!g_running) break;

        // [Enter] key (empty command)
        if (strlen(cmd) == 0) {
            dirs = LoadDirs(); // Re-load dirs from file
            StringList files = GetAllFiles(&dirs);
            if (files.count == 0) {
                WriteColor(COLOR_RED, "[!!!] I have no idea where to look! Be my guest, give me a clue!\n");
            } else {
                // Pick a random index from the file list
                int index = rand() % files.count;
                // Store the chosen file path in the global variable
                strncpy(g_LastShownFile, files.items[index], PATH_MAX - 1);
                
                char buffer[PATH_MAX + 32];
                snprintf(buffer, sizeof(buffer), "%s%s\n", COLOR_LIGHT_BLUE, g_LastShownFile);
                WriteColor(buffer, ""); // Print the colored path
            }
            FreeStringList(&files);
        }
        // Logic for the "newdir" command
        else if (strcmp(cmd, "newdir") == 0) {
            printf("Enter directory path: ");
            if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;
            cmd[strcspn(cmd, "\r\n")] = '\0';

            // Check if path is a valid directory
            struct stat st;
            if (stat(cmd, &st) != 0 || !S_ISDIR(st.st_mode)) {
                WriteColor(COLOR_RED, "Invalid directory path or path not found.\n");
            } else {
                // Check if directory is already in the list
                int found = 0;
                for (int i = 0; i < dirs.count; i++) {
                    if (strcmp(dirs.items[i], cmd) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    WriteColor(COLOR_RED, "Directory already in list.\n");
                } else {
                    AddStringToList(&dirs, cmd);
                    SaveDirs(&dirs);
                    char buffer[PATH_MAX + 32];
                    snprintf(buffer, sizeof(buffer), "[+] Added: %s\n", cmd);
                    WriteColor(COLOR_GREEN, buffer);
                }
            }
        }
        // Logic for the "removedir" command
        else if (strcmp(cmd, "removedir") == 0) {
            if (dirs.count == 0) {
                WriteColor(COLOR_RED, "[!!!] No directories to remove.\n");
                continue;
            }
            // List all directories with 1-based index
            printf("Saved directories:\n");
            for (int i = 0; i < dirs.count; i++) {
                printf("%d. %s\n", i + 1, dirs.items[i]);
            }
            printf("Enter index or path to remove: ");
            if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;
            cmd[strcspn(cmd, "\r\n")] = '\0';

            long idx = strtol(cmd, NULL, 10) - 1; // Try parsing as 1-based index
            int removed = 0;

            // Try removing by index
            if (idx >= 0 && idx < dirs.count) {
                char buffer[PATH_MAX + 32];
                snprintf(buffer, sizeof(buffer), "Removed: %s\n", dirs.items[idx]);
                WriteColor(COLOR_RED, buffer);
                
                free(dirs.items[idx]); // Free the string
                // Shift all subsequent items down in the array
                for (int i = (int)idx; i < dirs.count - 1; i++) {
                    dirs.items[i] = dirs.items[i + 1];
                }
                dirs.count--;
                removed = 1;
            } else {
                // Try removing by path
                for (int i = 0; i < dirs.count; i++) {
                    if (strcmp(dirs.items[i], cmd) == 0) {
                        char buffer[PATH_MAX + 32];
                        snprintf(buffer, sizeof(buffer), "Removed: %s\n", dirs.items[i]);
                        WriteColor(COLOR_RED, buffer);
                        
                        free(dirs.items[i]);
                        // Shift items
                        for (int j = i; j < dirs.count - 1; j++) {
                            dirs.items[j] = dirs.items[j + 1];
                        }
                        dirs.count--;
                        removed = 1;
                        break;
                    }
                }
            }
            
            if (removed) {
                SaveDirs(&dirs);
            } else {
                printf("Invalid index or directory not found in list.\n");
            }
        }
        // Logic for the "viewdir" command
        else if (strcmp(cmd, "viewdir") == 0) {
            dirs = LoadDirs(); // Re-load
            if (dirs.count == 0) {
                WriteColor(COLOR_RED, "[!!!] No directories saved yet.\n");
            } else {
                WriteColor(COLOR_CYAN, "Saved directories:\n");
                for (int i = 0; i < dirs.count; i++) {
                    printf(" - %s\n", dirs.items[i]);
                }
            }
        }
        // Logic for the "open" command
        else if (strcmp(cmd, "open") == 0) {
            HandleOpenCommand();
        }
        // Logic for the "exit" command
        else if (strcmp(cmd, "exit") == 0) {
            printf("Farewell.\n");
            g_running = 0; // Signal exit
        }
        else {
            printf("Unknown command.\n");
        }
    }

    // --- Cleanup ---
    g_running = 0; // Ensure the global running flag is set to false

    // Force the watcher thread to terminate (it may be blocked on read)
    pthread_cancel(watcherThreadID);
    // Wait for the thread to actually finish
    pthread_join(watcherThreadID, NULL);
    
    FreeStringList(&dirs);
    printf(COLOR_RESET); // Reset terminal color
    return 0;
}

// Signal handler for Ctrl+C (SIGINT)
void SignalHandler(int signum) {
    if (signum == SIGINT) {
        WriteColor(COLOR_YELLOW, "\nInterrupted by user. Exiting...\n");
        g_running = 0; // Set the global flag to stop all loops
    }
}


// ====================================================================
// --- StringList (Dynamic String Array) Helpers ---
// ====================================================================

// Initializes an empty StringList
void InitStringList(StringList* list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

// Adds a copy of a string to the list, resizing if necessary
void AddStringToList(StringList* list, const char* str) {
    // If the list is full, double its capacity
    if (list->count == list->capacity) {
        int newCapacity = (list->capacity == 0) ? 8 : list->capacity * 2;
        char** newItems = (char**)realloc(list->items, newCapacity * sizeof(char*));
        if (newItems == NULL) {
            WriteColor(COLOR_RED, "Fatal: Out of memory in AddStringToList.\n");
            exit(1);
        }
        list->items = newItems;
        list->capacity = newCapacity;
    }
    // strdup creates a heap-allocated copy of the string
    list->items[list->count] = strdup(str);
    if (list->items[list->count] == NULL) {
        WriteColor(COLOR_RED, "Fatal: Out of memory in strdup.\n");
        exit(1);
    }
    list->count++;
}

// Frees all strings inside the list and the list's items array
void FreeStringList(StringList* list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    InitStringList(list); // Reset to a clean state
}

// ====================================================================
// --- Utility Functions ---
// ====================================================================

// Reads the list of directories from "dirs.txt"
StringList LoadDirs() {
    StringList dirs;
    InitStringList(&dirs);
    FILE* fp = fopen(DIRS_FILE, "r"); // Open for reading

    if (fp == NULL) {
        // File doesn't exist, create an empty one.
        fp = fopen(DIRS_FILE, "w");
        if (fp) fclose(fp);
        return dirs; // Return empty list
    }

    char buffer[PATH_MAX];
    while (fgets(buffer, PATH_MAX, fp)) {
        buffer[strcspn(buffer, "\r\n")] = '\0'; // Trim newline
        // Add non-empty lines to the list
        if (strlen(buffer) > 0) {
            AddStringToList(&dirs, buffer);
        }
    }
    fclose(fp);
    return dirs;
}

// Writes the current list of directories back to "dirs.txt"
void SaveDirs(const StringList* dirs) {
    FILE* fp = fopen(DIRS_FILE, "w"); // Open for writing
    if (fp) {
        for (int i = 0; i < dirs->count; i++) {
            fprintf(fp, "%s\n", dirs->items[i]);
        }
        fclose(fp);
    }
}

// Recursively scans a directory and adds all files to the list
void GetAllFilesRecursive(const char* basePath, StringList* fileList) {
    DIR* dir = opendir(basePath);
    if (!dir) {
        // Error (e.g., permissions denied)
        char buffer[PATH_MAX + 64];
        snprintf(buffer, sizeof(buffer), "Warning: Access denied to directory %s. Skipping.\n", basePath);
        WriteColor(COLOR_YELLOW, buffer);
        return;
    }

    struct dirent* entry;
    // Read all entries in the directory
    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Construct the full path
        char fullPath[PATH_MAX];
        snprintf(fullPath, PATH_MAX, "%s/%s", basePath, entry->d_name);

                // Check if the entry is a directory or file
        #if defined(DT_DIR) && defined(DT_REG) && defined(DT_UNKNOWN)
                if (entry->d_type == DT_DIR) {
                    // It's a directory, recurse
                    GetAllFilesRecursive(fullPath, fileList);
                } else if (entry->d_type == DT_REG) {
                    // It's a regular file, add it
                    AddStringToList(fileList, fullPath);
                } else if (entry->d_type == DT_UNKNOWN) {
                    // Filesystem doesn't supply d_type, use stat() as a fallback
                    struct stat st;
                    if (stat(fullPath, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            GetAllFilesRecursive(fullPath, fileList);
                        } else if (S_ISREG(st.st_mode)) {
                            AddStringToList(fileList, fullPath);
                        }
                    }
                }
        #else
                {
                    // d_type constants not available on this platform; always use stat()
                    struct stat st;
                    if (stat(fullPath, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            GetAllFilesRecursive(fullPath, fileList);
                        } else if (S_ISREG(st.st_mode)) {
                            AddStringToList(fileList, fullPath);
                        }
                    } else {
                        char buffer[PATH_MAX + 64];
                        snprintf(buffer, sizeof(buffer), "Warning: stat failed for %s: %s. Skipping.\n", fullPath, strerror(errno));
                        WriteColor(COLOR_YELLOW, buffer);
                    }
                }
        #endif
    }
    closedir(dir);
}

// Gets a list of all files from all saved directories
StringList GetAllFiles(const StringList* dirs) {
    StringList files;
    InitStringList(&files);
    // Iterate over each base directory and start the recursive scan
    for (int i = 0; i < dirs->count; i++) {
        struct stat st;
        if (stat(dirs->items[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            GetAllFilesRecursive(dirs->items[i], &files);
        }
    }
    return files;
}

// Helper function to print text with a specific ANSI color
void WriteColor(const char* color, const char* message) {
    printf("%s%s" COLOR_RESET, color, message);
}

// Handles the "open" command logic
void HandleOpenCommand() {
    if (strlen(g_LastShownFile) == 0) {
        WriteColor(COLOR_YELLOW, "No file has been displayed yet. Press Enter first.\n");
        return;
    }

    // Check if the file still exists using access()
    if (access(g_LastShownFile, F_OK) != 0) {
        char buffer[PATH_MAX + 64];
        snprintf(buffer, sizeof(buffer), "File not found. It may have been moved or deleted: %s\n", g_LastShownFile);
        WriteColor(COLOR_RED, buffer);
        return;
    }

    // --- Use 'xdg-open' to open the file with the default app ---
    
    // 1. We must escape the filename for the shell to prevent injection attacks.
    // 2. Allocate a buffer large enough for the escaped path.
    //    (Worst case: every char is ' + \, so 4*len + 3 for quotes and null)
    char* escapedPath = (char*)malloc(strlen(g_LastShownFile) * 4 + 3);
    if (!escapedPath) {
        WriteColor(COLOR_RED, "Failed to allocate memory for 'open' command.\n");
        return;
    }
    
    // 3. Simple shell escape: wrap in ' and replace any internal ' with '\''.
    const char* p_in = g_LastShownFile;
    char* p_out = escapedPath;
    *p_out++ = '\''; // Start with a quote
    while (*p_in) {
        if (*p_in == '\'') {
            *p_out++ = '\''; // ' -> '
            *p_out++ = '\\'; // \
            *p_out++ = '\''; // '
            *p_out++ = '\''; // '
        } else {
            *p_out++ = *p_in;
        }
        p_in++;
    }
    *p_out++ = '\''; // End with a quote
    *p_out = '\0';
    
    // 4. Build the full command (e.g., "xdg-open '/path/to/file'")
    //    xdg-open is the Linux standard for opening a file with its default app.
    char cmd[PATH_MAX * 4 + 16]; // Extra space for "xdg-open "
    snprintf(cmd, sizeof(cmd), "xdg-open %s", escapedPath);
    free(escapedPath);
    
    // 5. Run the command using system()
    int ret = system(cmd);
    if (ret == 0) {
        char buffer[PATH_MAX + 32];
        snprintf(buffer, sizeof(buffer), "Opening: %s\n", g_LastShownFile);
        WriteColor(COLOR_GREEN, buffer);
    } else {
        char buffer[PATH_MAX + 64];
        snprintf(buffer, sizeof(buffer), "Failed to open file. Is 'xdg-open' installed? (code %d)\n", ret);
        WriteColor(COLOR_RED, buffer);
    }
}


// ====================================================================
// --- File Watcher Thread ---
// ====================================================================

// This function runs in a separate thread to watch for file changes
void* WatcherThread(void* arg) {
    StringList* dirs = (StringList*)arg;
    int fd; // File descriptor for the inotify instance

    // We need to map inotify's "watch descriptors" (wd) back to directory names
    #define MAX_WATCHES 1024
    char* wd_map[MAX_WATCHES] = {NULL};
    
    // Initialize the inotify system. IN_NONBLOCK means 'read' won't block.
    fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
        perror(COLOR_RED "[Watcher] inotify_init1 failed" COLOR_RESET);
        return NULL;
    }

    // Add a "watch" for each directory in our list
    int watch_count = 0;
    for (int i = 0; i < dirs->count && watch_count < MAX_WATCHES; i++) {
        // We watch for create, delete, and move events
        int wd = inotify_add_watch(fd, dirs->items[i], 
                                  IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
        if (wd < 0) {
            fprintf(stderr, COLOR_RED "[Watcher] Could not watch %s: %s\n" COLOR_RESET, dirs->items[i], strerror(errno));
        } else {
            wd_map[wd] = dirs->items[i]; // Map the wd (int) to the path (char*)
            watch_count++;
        }
    }
    
    if (watch_count == 0) {
        WriteColor(COLOR_RED, "[Watcher] No valid directories to watch. Thread exiting.\n");
        close(fd);
        return NULL;
    }

    // Buffer to read events into
    #define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + 16))
    char buffer[EVENT_BUF_LEN];

    // Loop until the main thread sets g_running to 0
    while (g_running) {
        // Read events from the inotify file descriptor
        int length = read(fd, buffer, EVENT_BUF_LEN);

        if (length < 0 && errno == EAGAIN) {
            // No events right now. Wait a bit and check g_running again.
            usleep(250000); // 250ms
            continue;
        } else if (length < 0) {
            // A real error occurred
            perror(COLOR_RED "[Watcher] read error" COLOR_RESET);
            break;
        }

        // Process all events that are in the buffer
        int i = 0;
        while (i < length) {
            struct inotify_event* event = (struct inotify_event*)&buffer[i];
            if (event->len) {
                // Ignore events for directories themselves
                if (!(event->mask & IN_ISDIR)) {
                    // Print messages based on the event type
                    if (event->mask & IN_CREATE) {
                        char msg[PATH_MAX + 64];
                        snprintf(msg, sizeof(msg), "[+] %s gestures a salutation!\n", event->name);
                        WriteColor(COLOR_GREEN, msg);
                    } else if (event->mask & IN_DELETE) {
                        char msg[PATH_MAX + 64];
                        snprintf(msg, sizeof(msg), "[-] %s bid farewell.\n", event->name);
                        WriteColor(COLOR_RED, msg);
                    } else if (event->mask & IN_MOVED_FROM) {
                        char msg[PATH_MAX + 64];
                        snprintf(msg, sizeof(msg), "[?] %s had changed its identity.\n", event->name);
                        WriteColor(COLOR_MAGENTA, msg);
                    } else if (event->mask & IN_MOVED_TO) {
                        char msg[PATH_MAX + 64];
                        snprintf(msg, sizeof(msg), "    ==> %s\n", event->name);
                        printf("%s", msg); // No color reset
                    }
                }
            }
            // Move to the next event in the buffer
            i += sizeof(struct inotify_event) + event->len;
        }
    }

    // Clean up the inotify file descriptor
    close(fd);
    return NULL;
}