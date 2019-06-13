enum dependency_type {
   DEP_READ,
   DEP_MODIFY,
   DEP_CREATE,
   DEP_REMOVE
};

struct file_reference {
    // fd may be AT_FDCWD and path may be NULL, but not both. If both are present, then
    // path is relative with respect to the directory in fd.
    int fd;
    char* path;
    // Whether to follow the link if the reference points at a symlink
    bool follow_links;
};

// Note that all strings (char*) passed to the following functions are transferring ownership,
// so the callee is responsible for freeing them. This is not true of the paths inside the
// file references: those will be freed shortly after the trace_add_* function is called.
void trace_add_dependency(pid_t thread_id, struct file_reference file, enum dependency_type type);
void trace_add_change_cwd(pid_t thread_id, struct file_reference file);
void trace_add_change_root(pid_t thread_id, struct file_reference file);
void trace_add_open(pid_t thread_id, int fd, struct file_reference file, int access_mode, bool is_rewrite);
void trace_add_pipe(pid_t thread_id, int fds[2]);
void trace_add_dup(pid_t thread_id, int duped_fd, int new_fd);
void trace_add_mmap(pid_t thread_id, int fd);
void trace_add_close(pid_t thread_id, int fd);
void trace_add_fork(pid_t parent_thread_id, pid_t child_process_id);
void trace_add_exec(pid_t process_id, char* exe_path);
void trace_add_exec_argument(pid_t process_id, char* argument, int index);
void trace_add_exit(pid_t thread_id);
