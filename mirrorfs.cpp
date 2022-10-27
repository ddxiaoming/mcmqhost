/* Adapted from
 * https://github.com/libfuse/libfuse/blob/master/example/passthrough_hp.cc */

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 12)

#include <dirent.h>
#include <fuse_lowlevel.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <mutex>

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

using cxxopts::OptionException;

typedef std::pair<ino_t, dev_t> SrcId;

namespace std {
template <> struct hash<SrcId> {
    size_t operator()(const SrcId& id) const
    {
        return hash<ino_t>{}(id.first) ^ hash<dev_t>{}(id.second);
    }
};
} // namespace std

struct Inode {
    int fd{-1};
    dev_t src_dev{0};
    ino_t src_ino{0};
    int generation{0};
    uint64_t nopen{0};
    uint64_t nlookup{0};
    std::mutex m;

    // Delete copy constructor and assignments. We could implement
    // move if we need it.
    Inode() = default;
    Inode(const Inode&) = delete;
    Inode(Inode&& inode) = delete;
    Inode& operator=(Inode&& inode) = delete;
    Inode& operator=(const Inode&) = delete;

    ~Inode()
    {
        if (fd > 0) close(fd);
    }
};

typedef std::unordered_map<SrcId, Inode> InodeMap;

struct Fs {
    std::mutex mutex;
    InodeMap inodes;
    Inode root;
    double timeout;
    bool debug;
    std::string source;
    size_t blocksize;
    dev_t src_dev;
    bool nosplice;
    bool nocache;
};
static Fs fs{};

static Inode& get_inode(fuse_ino_t ino)
{
    if (ino == FUSE_ROOT_ID) return fs.root;

    Inode* inode = reinterpret_cast<Inode*>(ino);
    if (inode->fd == -1) {
        spdlog::error("Unknown inode {}", ino);
        abort();
    }
    return *inode;
}

static int get_fs_fd(fuse_ino_t ino)
{
    int fd = get_inode(ino).fd;
    return fd;
}

static void mfs_getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
{
    (void)fi;
    Inode& inode = get_inode(ino);
    struct stat attr;
    auto res =
        fstatat(inode.fd, "", &attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (res == -1) {
        fuse_reply_err(req, errno);
        return;
    }
    fuse_reply_attr(req, &attr, fs.timeout);
}

static void do_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                       int valid, struct fuse_file_info* fi)
{
    Inode& inode = get_inode(ino);
    int ifd = inode.fd;
    int res;

    if (valid & FUSE_SET_ATTR_MODE) {
        if (fi) {
            res = fchmod(fi->fh, attr->st_mode);
        } else {
            char procname[64];
            sprintf(procname, "/proc/self/fd/%i", ifd);
            res = chmod(procname, attr->st_mode);
        }
        if (res == -1) goto out_err;
    }
    if (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
        uid_t uid =
            (valid & FUSE_SET_ATTR_UID) ? attr->st_uid : static_cast<uid_t>(-1);
        gid_t gid =
            (valid & FUSE_SET_ATTR_GID) ? attr->st_gid : static_cast<gid_t>(-1);

        res = fchownat(ifd, "", uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
        if (res == -1) goto out_err;
    }
    if (valid & FUSE_SET_ATTR_SIZE) {
        if (fi) {
            res = ftruncate(fi->fh, attr->st_size);
        } else {
            char procname[64];
            sprintf(procname, "/proc/self/fd/%i", ifd);
            res = truncate(procname, attr->st_size);
        }
        if (res == -1) goto out_err;
    }
    if (valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
        struct timespec tv[2];

        tv[0].tv_sec = 0;
        tv[1].tv_sec = 0;
        tv[0].tv_nsec = UTIME_OMIT;
        tv[1].tv_nsec = UTIME_OMIT;

        if (valid & FUSE_SET_ATTR_ATIME_NOW)
            tv[0].tv_nsec = UTIME_NOW;
        else if (valid & FUSE_SET_ATTR_ATIME)
            tv[0] = attr->st_atim;

        if (valid & FUSE_SET_ATTR_MTIME_NOW)
            tv[1].tv_nsec = UTIME_NOW;
        else if (valid & FUSE_SET_ATTR_MTIME)
            tv[1] = attr->st_mtim;

        if (fi)
            res = futimens(fi->fh, tv);
        else {
            char procname[64];
            sprintf(procname, "/proc/self/fd/%i", ifd);
            res = utimensat(AT_FDCWD, procname, tv, 0);
        }
        if (res == -1) goto out_err;
    }
    return mfs_getattr(req, ino, fi);

out_err:
    fuse_reply_err(req, errno);
}

static void mfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                        int valid, fuse_file_info* fi)
{
    (void)ino;
    do_setattr(req, ino, attr, valid, fi);
}

static int do_lookup(fuse_ino_t parent, const char* name, fuse_entry_param* e)
{
    spdlog::debug("lookup(): name={}, parent={}", name, parent);

    memset(e, 0, sizeof(*e));
    e->attr_timeout = fs.timeout;
    e->entry_timeout = fs.timeout;

    auto newfd = openat(get_fs_fd(parent), name, O_PATH | O_NOFOLLOW);
    if (newfd == -1) return errno;

    auto res =
        fstatat(newfd, "", &e->attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (res == -1) {
        auto saveerr = errno;
        close(newfd);
        spdlog::debug("lookup(): fstatat failed");
        return saveerr;
    }

    if (e->attr.st_dev != fs.src_dev) {
        spdlog::warn(
            "Mountpoints in the source directory tree will be hidden.");
        return ENOTSUP;
    } else if (e->attr.st_ino == FUSE_ROOT_ID) {
        spdlog::error("Source directory tree must not include {}",
                      FUSE_ROOT_ID);
        return EIO;
    }

    SrcId id{e->attr.st_ino, e->attr.st_dev};
    std::unique_lock<std::mutex> fs_lock{fs.mutex};
    Inode* inode_p;
    try {
        inode_p = &fs.inodes[id];
    } catch (std::bad_alloc&) {
        return ENOMEM;
    }
    e->ino = reinterpret_cast<fuse_ino_t>(inode_p);
    Inode& inode{*inode_p};
    e->generation = inode.generation;

    if (inode.fd == -ENOENT) { // found unlinked inode
        spdlog::debug("lookup(): inode {} recycled; generation={}",
                      e->attr.st_ino, inode.generation);
        /* fallthrough to new inode but keep existing inode.nlookup */
    }

    if (inode.fd > 0) { // found existing inode
        fs_lock.unlock();
        spdlog::debug("lookup(): inode {} (userspace) already known; fd={}",
                      e->attr.st_ino, inode.fd);
        std::lock_guard<std::mutex> g{inode.m};

        inode.nlookup++;
        spdlog::debug("lookup(): inode {} count {}", inode.src_ino,
                      inode.nlookup);

        close(newfd);
    } else { // no existing inode
        /* This is just here to make Helgrind happy. It violates the
           lock ordering requirement (inode.m must be acquired before
           fs.mutex), but this is of no consequence because at this
           point no other thread has access to the inode mutex */
        std::lock_guard<std::mutex> g{inode.m};
        inode.src_ino = e->attr.st_ino;
        inode.src_dev = e->attr.st_dev;

        inode.nlookup++;
        spdlog::debug("lookup(): inode {} count {}", inode.src_ino,
                      inode.nlookup);

        inode.fd = newfd;
        fs_lock.unlock();

        spdlog::debug("lookup(): created userspace inode {}; fd={}",
                      e->attr.st_ino, inode.fd);
    }

    return 0;
}

static void mfs_lookup(fuse_req_t req, fuse_ino_t parent, const char* name)
{
    fuse_entry_param e{};
    auto err = do_lookup(parent, name, &e);
    if (err == ENOENT) {
        e.attr_timeout = fs.timeout;
        e.entry_timeout = fs.timeout;
        e.ino = e.attr.st_ino = 0;
        fuse_reply_entry(req, &e);
    } else if (err) {
        if (err == ENFILE || err == EMFILE)
            spdlog::error("Reached maximum number of file descriptors");
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
}

static void forget_one(fuse_ino_t ino, uint64_t n)
{
    Inode& inode = get_inode(ino);
    std::unique_lock<std::mutex> l{inode.m};

    if (n > inode.nlookup) {
        spdlog::error("Negative lookup count for inode {}", inode.src_ino);
        abort();
    }
    inode.nlookup -= n;

    spdlog::debug("forget_one(): inode {} count {}", inode.src_ino,
                  inode.nlookup);

    if (!inode.nlookup) {
        spdlog::debug("forget: cleaning up inode {}", inode.src_ino);

        {
            std::lock_guard<std::mutex> g_fs{fs.mutex};
            l.unlock();
            fs.inodes.erase({inode.src_ino, inode.src_dev});
        }
    } else
        spdlog::debug("forget: inode {} lookup count now {}", inode.src_ino,
                      inode.nlookup);
}

static void mfs_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
    forget_one(ino, nlookup);
    fuse_reply_none(req);
}

static void mfs_forget_multi(fuse_req_t req, size_t count,
                             fuse_forget_data* forgets)
{
    for (int i = 0; i < count; i++)
        forget_one(forgets[i].ino, forgets[i].nlookup);
    fuse_reply_none(req);
}

struct DirHandle {
    DIR* dp{nullptr};
    off_t offset;

    DirHandle() = default;
    DirHandle(const DirHandle&) = delete;
    DirHandle& operator=(const DirHandle&) = delete;

    ~DirHandle()
    {
        if (dp) closedir(dp);
    }
};

static DirHandle* get_dir_handle(fuse_file_info* fi)
{
    return reinterpret_cast<DirHandle*>(fi->fh);
}

static void mfs_opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
{
    Inode& inode = get_inode(ino);
    auto d = new (std::nothrow) DirHandle;
    if (d == nullptr) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    std::lock_guard<std::mutex> g{inode.m};

    auto fd = openat(inode.fd, ".", O_RDONLY);
    if (fd == -1) goto out_errno;

    d->dp = fdopendir(fd);
    if (d->dp == nullptr) goto out_errno;

    d->offset = 0;

    fi->fh = reinterpret_cast<uint64_t>(d);
    if (fs.timeout) {
        fi->keep_cache = 1;
        fi->cache_readdir = 1;
    }
    fuse_reply_open(req, fi);
    return;

out_errno:
    auto error = errno;
    delete d;
    if (error == ENFILE || error == EMFILE)
        spdlog::error("Reached maximum number of file descriptors");
    fuse_reply_err(req, error);
}

static bool is_dot_or_dotdot(const char* name)
{
    return name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static void do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t offset, fuse_file_info* fi, int plus)
{
    auto d = get_dir_handle(fi);
    Inode& inode = get_inode(ino);
    std::lock_guard<std::mutex> g{inode.m};
    char* p;
    auto rem = size;
    int err = 0, count = 0;

    spdlog::debug("readdir(): started with offset {}", offset);

    auto buf = new (std::nothrow) char[size];
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }
    p = buf;

    if (offset != d->offset) {
        spdlog::debug("readdir(): seeking to {}", offset);
        seekdir(d->dp, offset);
        d->offset = offset;
    }

    while (1) {
        struct dirent* entry;
        errno = 0;
        entry = readdir(d->dp);
        if (!entry) {
            if (errno) {
                err = errno;
                spdlog::warn("readdir(): readdir failed with {}",
                             strerror(errno));
                goto error;
            }
            break; // End of stream
        }
        d->offset = entry->d_off;
        if (is_dot_or_dotdot(entry->d_name)) continue;

        fuse_entry_param e{};
        size_t entsize;
        if (plus) {
            err = do_lookup(ino, entry->d_name, &e);
            if (err) goto error;
            entsize = fuse_add_direntry_plus(req, p, rem, entry->d_name, &e,
                                             entry->d_off);

            if (entsize > rem) {
                spdlog::debug("readdir(): buffer full, returning data");
                forget_one(e.ino, 1);
                break;
            }
        } else {
            e.attr.st_ino = entry->d_ino;
            e.attr.st_mode = entry->d_type << 12;
            entsize = fuse_add_direntry(req, p, rem, entry->d_name, &e.attr,
                                        entry->d_off);

            if (entsize > rem) {
                spdlog::debug("readdir(): buffer full, returning data");
                break;
            }
        }

        p += entsize;
        rem -= entsize;
        count++;
        spdlog::debug("readdir(): added to buffer: {}, ino {}, offset {}",
                      entry->d_name, e.attr.st_ino, entry->d_off);
    }
    err = 0;
error:

    // If there's an error, we can only signal it if we haven't stored
    // any entries yet - otherwise we'd end up with wrong lookup
    // counts for the entries that are already in the buffer. So we
    // return what we've collected until that point.
    if (err && rem == size) {
        if (err == ENFILE || err == EMFILE)
            spdlog::error("Reached maximum number of file descriptors");
        fuse_reply_err(req, err);
    } else {
        spdlog::debug("readdir(): returing {} entries, curr offset {}", count,
                      d->offset);
        fuse_reply_buf(req, buf, size - rem);
    }
    delete[] buf;
    return;
}

static void mfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                        off_t offset, fuse_file_info* fi)
{
    do_readdir(req, ino, size, offset, fi, 0);
}

static void mfs_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                            off_t offset, fuse_file_info* fi)
{
    do_readdir(req, ino, size, offset, fi, 1);
}

static void mfs_releasedir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
{
    (void)ino;
    auto d = get_dir_handle(fi);
    delete d;
    fuse_reply_err(req, 0);
}

static void mfs_create(fuse_req_t req, fuse_ino_t parent, const char* name,
                       mode_t mode, fuse_file_info* fi)
{
    Inode& inode_p = get_inode(parent);

    auto fd =
        openat(inode_p.fd, name, (fi->flags | O_CREAT) & ~O_NOFOLLOW, mode);
    if (fd == -1) {
        auto err = errno;
        if (err == ENFILE || err == EMFILE)
            spdlog::error("Reached maximum number of file descriptors");
        fuse_reply_err(req, err);
        return;
    }

    fi->fh = fd;
    fuse_entry_param e;
    auto err = do_lookup(parent, name, &e);
    if (err) {
        if (err == ENFILE || err == EMFILE)
            spdlog::error("Reached maximum number of file descriptors");
        fuse_reply_err(req, err);
        return;
    }

    Inode& inode = get_inode(e.ino);
    std::lock_guard<std::mutex> g{inode.m};
    inode.nopen++;
    fuse_reply_create(req, &e, fi);
}

static void mfs_open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
{
    Inode& inode = get_inode(ino);

    /* With writeback cache, kernel may send read requests even
       when userspace opened write-only */
    if (fs.timeout && (fi->flags & O_ACCMODE) == O_WRONLY) {
        fi->flags &= ~O_ACCMODE;
        fi->flags |= O_RDWR;
    }

    /* With writeback cache, O_APPEND is handled by the kernel.  This
       breaks atomicity (since the file may change in the underlying
       filesystem, so that the kernel's idea of the end of the file
       isn't accurate anymore). However, no process should modify the
       file in the underlying filesystem once it has been read, so
       this is not a problem. */
    if (fs.timeout && fi->flags & O_APPEND) fi->flags &= ~O_APPEND;

    /* Unfortunately we cannot use inode.fd, because this was opened
       with O_PATH (so it doesn't allow read/write access). */
    char buf[64];
    sprintf(buf, "/proc/self/fd/%i", inode.fd);
    auto fd = open(buf, fi->flags & ~O_NOFOLLOW);
    if (fd == -1) {
        auto err = errno;
        if (err == ENFILE || err == EMFILE)
            spdlog::error("Reached maximum number of file descriptors");
        fuse_reply_err(req, err);
        return;
    }

    std::lock_guard<std::mutex> g{inode.m};
    inode.nopen++;
    fi->keep_cache = (fs.timeout != 0);
    fi->noflush = (fs.timeout == 0 && (fi->flags & O_ACCMODE) == O_RDONLY);
    fi->fh = fd;
    fuse_reply_open(req, fi);
}

static void mfs_release(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi)
{
    Inode& inode = get_inode(ino);
    std::lock_guard<std::mutex> g{inode.m};
    inode.nopen--;
    close(fi->fh);
    fuse_reply_err(req, 0);
}

cxxopts::ParseResult parse_arguments(int argc, char* argv[])
{
    try {
        cxxopts::Options options(argv[0], " - Mirror filesystem");

        // clang-format off
        options.add_options()
            ("debug-fuse", "Enable libfuse debug messages")
            ("mountpoint", "Mount point", cxxopts::value<std::string>())
            ("m,mirror", "Mirror directory", cxxopts::value<std::string>())
            ("N,max-threads", "Maximum number of threads",cxxopts::value<int>()->default_value("8"))
            ("h,help", "Print help");
        // clang-format on

        options.parse_positional({"mountpoint"});

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cerr << options.help() << std::endl;
            exit(EXIT_SUCCESS);
        }

        return result;
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse arguments: {}", e.what());
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char* argv[])
{
    struct fuse_loop_config* loop_config = NULL;

    spdlog::cfg::load_env_levels();

    auto options = parse_arguments(argc, argv);
    int ret;

    int max_threads;
    std::string mountpoint, mirror;
    try {
        max_threads = options["max-threads"].as<int>();
        mountpoint = options["mountpoint"].as<std::string>();
        mirror = options["mirror"].as<std::string>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    char* resolved_path = realpath(mirror.c_str(), NULL);
    if (resolved_path == NULL)
        spdlog::warn("realpath() failed with {}", strerror(errno));
    fs.source = std::string{resolved_path};
    free(resolved_path);

    // Initialize filesystem root
    fs.root.fd = -1;
    fs.root.nlookup = 9999;
    fs.timeout = 0;

    struct stat stat;
    ret = lstat(fs.source.c_str(), &stat);
    if (ret == -1) spdlog::error("Failed to stat source (\"{}\")", fs.source);
    if (!S_ISDIR(stat.st_mode)) spdlog::error("Source is not a directory");
    fs.src_dev = stat.st_dev;

    fs.root.fd = open(fs.source.c_str(), O_PATH);
    if (fs.root.fd == -1) spdlog::error("Failed to open source directory");

    fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    if (fuse_opt_add_arg(&args, argv[0]) || fuse_opt_add_arg(&args, "-o") ||
        fuse_opt_add_arg(&args, "default_permissions,fsname=mirror") ||
        (options.count("debug-fuse") && fuse_opt_add_arg(&args, "-odebug"))) {
        spdlog::error("Out of memory creating FUSE arguments");
        return EXIT_FAILURE;
    }

    fuse_lowlevel_ops mfs_oper{
        .lookup = mfs_lookup,
        .forget = mfs_forget,
        .getattr = mfs_getattr,
        .setattr = mfs_setattr,
        .open = mfs_open,
        .release = mfs_release,
        .opendir = mfs_opendir,
        .readdir = mfs_readdir,
        .releasedir = mfs_releasedir,
        .create = mfs_create,
        .forget_multi = mfs_forget_multi,
        .readdirplus = mfs_readdirplus,
    };

    auto se = fuse_session_new(&args, &mfs_oper, sizeof(mfs_oper), nullptr);
    if (!se) {
        spdlog::error("Failed to create session");
        goto err_out1;
    }

    if (fuse_set_signal_handlers(se) != 0) goto err_out2;

    loop_config = fuse_loop_cfg_create();
    fuse_loop_cfg_set_max_threads(loop_config, max_threads);
    fuse_loop_cfg_set_idle_threads(loop_config, -1);

    if ((ret = fuse_session_mount(se, mountpoint.c_str())) != 0) {
        spdlog::error("Failed to mount filesystem: {}", strerror(ret));
        goto err_out3;
    }

    ret = fuse_session_loop_mt(se, loop_config);
    if (ret != 0) spdlog::error("Session loop error: {}", strerror(ret));

    fuse_session_unmount(se);

err_out3:
    fuse_remove_signal_handlers(se);
err_out2:
    fuse_session_destroy(se);
err_out1:
    fuse_loop_cfg_destroy(loop_config);
    fuse_opt_free_args(&args);

    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
