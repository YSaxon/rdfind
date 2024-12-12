/*
   copyright 2006-2017 Paul Dreik (earlier Paul Sundvall)
   Distributed under GPL v 2.0 or later, at your option.
   See LICENSE for further details.
*/

#include "config.h"

// std
#include <cassert>
#include <cerrno>   //for errno
#include <cstring>  //for strerror
#include <fstream>  //for file reading
#include <iostream> //for cout etc

// os
#include <sys/stat.h> //for file info
#include <unistd.h>   //for unlink etc.

// project
#include "Checksum.hh" //checksum calculation
#include "Fileinfo.hh"
#include "UndoableUnlink.hh"

#if defined(HAVE_APFS_CLONING)
#include <sys/clonefile.h>
#include <sys/mount.h>
#include <fcntl.h>        // for fcntl, F_LOG2PHYS, O_RDONLY
#include <sys/fcntl.h>    // for struct log2phys
#include <unistd.h>       // for lseek, SEEK_END
#endif

int
Fileinfo::fillwithbytes(enum readtobuffermode filltype,
                        enum readtobuffermode lasttype)
{

  // Decide if we are going to read from file or not.
  // If file is short, first bytes might be ALL bytes!
  if (lasttype != readtobuffermode::NOT_DEFINED) {
    if (this->size() <= static_cast<filesizetype>(m_somebytes.size())) {
      // pointless to read - all bytes in the file are in the field
      // m_somebytes, or checksum is calculated!
      return 0;
    }
  }

  // set memory to zero
  m_somebytes.fill('\0');

  std::fstream f1;
  f1.open(m_filename.c_str(), std::ios_base::in);
  if (!f1.is_open()) {
    std::cerr << "fillwithbytes.cc: Could not open file \"" << m_filename
              << "\"" << std::endl;
    return -1;
  }

  auto checksumtype = Checksum::checksumtypes::NOTSET;
  // read some bytes
  switch (filltype) {
    case readtobuffermode::READ_FIRST_BYTES:
      // read at start of file
      f1.read(m_somebytes.data(), SomeByteSize);
      break;
    case readtobuffermode::READ_LAST_BYTES:
      // read at end of file
      f1.seekg(-SomeByteSize, std::ios_base::end);
      f1.read(m_somebytes.data(), SomeByteSize);
      break;
    case readtobuffermode::CREATE_MD5_CHECKSUM:
      checksumtype = Checksum::checksumtypes::MD5;
      break;
    case readtobuffermode::CREATE_SHA1_CHECKSUM:
      checksumtype = Checksum::checksumtypes::SHA1;
      break;
    case readtobuffermode::CREATE_SHA256_CHECKSUM:
      checksumtype = Checksum::checksumtypes::SHA256;
      break;
    case readtobuffermode::CREATE_SHA512_CHECKSUM:
      checksumtype = Checksum::checksumtypes::SHA512;
      break;
    default:
      std::cerr << "does not know how to do that filltype:"
                << static_cast<long>(filltype) << std::endl;
  }

  if (checksumtype != Checksum::checksumtypes::NOTSET) {
    Checksum chk(checksumtype);

    char buffer[4096];
    while (f1) {
      f1.read(buffer, sizeof(buffer));
      // gcount is never negative, the cast is safe.
      chk.update(static_cast<std::size_t>(f1.gcount()), buffer);
    }

    // store the result of the checksum calculation in somebytes
    int digestlength = chk.getDigestLength();
    if (digestlength <= 0 ||
        digestlength >= static_cast<int>(m_somebytes.size())) {
      std::cerr << "wrong answer from getDigestLength! FIXME" << std::endl;
    }
    if (chk.printToBuffer(m_somebytes.data(), m_somebytes.size())) {
      std::cerr << "failed writing digest to buffer!!" << std::endl;
    }
  }

  return 0;
}

bool
Fileinfo::readfileinfo()
{
  struct stat info;
  m_info.is_file = false;
  m_info.is_directory = false;

  int res;
  do {
    res = stat(m_filename.c_str(), &info);
  } while (res < 0 && errno == EINTR);

  if (res < 0) {
    m_info.stat_size = 0;
    m_info.stat_ino = 0;
    m_info.stat_dev = 0;
    std::cerr << "readfileinfo.cc:Something went wrong when reading file "
                 "info from \""
              << m_filename << "\" :" << std::strerror(errno) << std::endl;
    return false;
  }

  // only keep the relevant information
  m_info.stat_size = info.st_size;
  m_info.stat_ino = info.st_ino;
  m_info.stat_dev = info.st_dev;

  m_info.is_file = S_ISREG(info.st_mode);
  m_info.is_directory = S_ISDIR(info.st_mode);
  return true;
}

const char*
Fileinfo::getduptypestring(const Fileinfo& A)
{

  switch (A.getduptype()) {
    case duptype::DUPTYPE_UNKNOWN:
      return "DUPTYPE_UNKNOWN";
    case duptype::DUPTYPE_FIRST_OCCURRENCE:
      return "DUPTYPE_FIRST_OCCURRENCE";
    case duptype::DUPTYPE_WITHIN_SAME_TREE:
      return "DUPTYPE_WITHIN_SAME_TREE";
    case duptype::DUPTYPE_OUTSIDE_TREE:
      return "DUPTYPE_OUTSIDE_TREE";
    default:
      assert("we should not get here!" == nullptr);
  }

  return "error-error";
}

// constructor
Fileinfo::Fileinfostat::Fileinfostat()
{
  stat_size = 99999;
  stat_ino = 99999;
  stat_dev = 99999;
  is_file = false;
  is_directory = false;
}

int
Fileinfo::deletefile()
{
  const int ret = unlink(name().c_str());
  if (ret) {
    std::cerr << "Failed deleting file " << name() << '\n';
  }
  return ret;
}

namespace {
int
simplifyPath(std::string& path)
{
  // replace a/./b with a/b
  do {
    const auto pos = path.find("/./");
    if (pos == std::string::npos) {
      break;
    }
    path.replace(pos, 3, "/");
  } while (true);

  // replace repeated slashes
  do {
    const auto pos = path.find("//");
    if (pos == std::string::npos) {
      break;
    }
    path.replace(pos, 2, "/");
  } while (true);

  // getting rid of /../ is difficult to get correct because of symlinks.
  // do not do it.
  return 0;
}

// prepares target, so that location can point to target in
// the best way possible
int
makeAbsolute(std::string& target)
{
  // if target is not absolute, let us make it absolute
  if (target.length() > 0 && target.at(0) == '/') {
    // absolute. do nothing.
  } else {
    // not absolute. make it absolute.

    // yes, this is possible to do with dynamically allocated memory,
    // but it is not portable then (and more work).
    // hmm, would it be possible to cache this and gain some speedup?
    const size_t buflength = 256;
    char buf[buflength];
    if (buf != getcwd(buf, buflength)) {
      std::cerr << "failed to get current working directory" << std::endl;
      return -1;
    }
    target = std::string(buf) + std::string("/") + target;
  }
  return 0;
}

/**
 * helper for transactional operation on a file. It will move the file to a
 * temporary, then invoke f with the filename as argument, then delete the
 * temporary. In case of failure at any point, it will do it's best to restore
 * the temporary back to the filename.
 * @param filename
 * @param f
 * @return zero on success.
 */
template<typename Func>
int
transactional_operation(const std::string& filename, const Func& f)
{
  // move the file to a temporary.
  UndoableUnlink restorer(filename);

  // did the move to a temporary go ok?
  if (!restorer.file_is_moved()) {
    return 1;
  }

  // yes, apply whatever the caller wanted.
  const int ret = f(filename);

  if (0 != ret) {
    // operation failed! rollback time (made by restorer at scope exit)
    return ret;
  }

  // operation succeeded, go ahead and unlink the temporary.
  if (0 != restorer.unlink()) {
    return 1;
  }

  return 0;
}
} // namespace

// makes a symlink that points to A
int
Fileinfo::makesymlink(const Fileinfo& A)
{
  const int retval =
    transactional_operation(name(), [&](const std::string& filename) {
      // The tricky thing is that the path must be correct, as seen from
      // the directory where *this is. Making the path absolute solves this
      // problem. Doing string manipulations to find how to make the path
      // relative is prone to error because directories can be symlinks.
      std::string target = A.name();
      makeAbsolute(target);
      // clean up the path, so it does not contain sequences "/./" or "//"
      simplifyPath(target);
      return symlink(target.c_str(), filename.c_str());
    });

  if (retval) {
    std::cerr << "Failed to make symlink " << name() << " to " << A.name()
              << '\n';
  }
  return retval;
}

// makes a hard link that points to A
int
Fileinfo::makehardlink(const Fileinfo& A)
{
  return transactional_operation(name(), [&](const std::string& filename) {
    // make a hardlink.
    const int retval = link(A.name().c_str(), filename.c_str());
    if (retval) {
      std::cerr << "Failed to make hardlink " << filename << " to " << A.name()
                << '\n';
    }
    return retval;
  });
}

int
Fileinfo::static_deletefile(Fileinfo& A, const Fileinfo& /*B*/)
{
  return A.deletefile();
}

int
Fileinfo::static_makesymlink(Fileinfo& A, const Fileinfo& B)
{
  return A.makesymlink(B);
}

int
Fileinfo::static_makehardlink(Fileinfo& A, const Fileinfo& B)
{
  return A.makehardlink(B);
}

#if defined(HAVE_APFS_CLONING)
#include <sys/clonefile.h>
#include <sys/mount.h>
#include <copyfile.h>

bool Fileinfo::is_on_apfs() const {
    struct statfs fs;
    if (statfs(m_filename.c_str(), &fs) != 0) {
        return false;
    }
    return strcmp(fs.f_fstypename, "apfs") == 0;
}

bool Fileinfo::is_clone_of(const Fileinfo& other) const {
  // Note that this function is only valid when files have already been shown to be identical, since all we are doing is comparing the very first blocks.
    return get_cow_id() == other.get_cow_id();
}

off_t Fileinfo::get_cow_id() const {
  // we can just look at the first block of the file
    if (!is_on_apfs()) {
        return -1;
    }

    int fd = open(m_filename.c_str(), O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct log2phys phys;
    off_t block_id = -1;

    if (fcntl(fd, F_LOG2PHYS, &phys) >= 0) {
        block_id = phys.l2p_devoffset;
    }

    close(fd);
    return block_id;
}

static std::string make_temp_file_nearby(const std::string &basePath) {
    // Extract directory from basePath
    std::string dir = basePath;
    size_t pos = dir.rfind('/');
    if (pos != std::string::npos) {
        dir.erase(pos);
    } else {
        // No slash found, use current directory
        dir = ".";
    }

    // Create a template for mkstemp
    std::string tmpl = dir + "/.clonebackup.XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    int fd = mkstemp(&buf[0]);
    if (fd == -1) {
        std::cerr << "Failed to create temporary file near " << basePath
                  << ": " << std::strerror(errno) << "\n";
        return std::string(); // Return empty on failure
    }
    close(fd); // We only need the name, not the open file descriptor
    return std::string(&buf[0]);
}

int Fileinfo::makeclone(const Fileinfo &other) {
    std::string target = name();
    std::string backup = make_temp_file_nearby(target);

    if (backup.empty()) {
        // Couldn't create a backup template file
        return -1;
    }

    // Step 1: Rename target to backup to preserve original file for rollback
    if (rename(target.c_str(), backup.c_str()) != 0) {
        std::cerr << "Failed to rename " << target << " to " << backup
                  << ": " << std::strerror(errno) << "\n";
        // Cleanup the unused backup file
        unlink(backup.c_str());
        return -1;
    }

    // Step 2: Perform the clone
    // Adjust flags if you want ACL copying by clonefile or no-owner-copy logic
    uint32_t clone_flags = 0;
    if (clonefile(other.name().c_str(), target.c_str(), clone_flags) != 0) {
        std::cerr << "Failed to clone " << other.name() << " into " << target
                  << ": " << std::strerror(errno) << "\n";

        // Rollback: restore the original target file
        if (rename(backup.c_str(), target.c_str()) != 0) {
            std::cerr << "Rollback error: failed to restore original file "
                      << target << " from " << backup << ": " << std::strerror(errno) << "\n";
        }
        return -1;
    }

    // Step 3: Use copyfile() to restore original metadata (ownership, perms, ACLs, xattrs)
    // COPYFILE_METADATA copies all non-data metadata
    if (copyfile(backup.c_str(), target.c_str(), NULL, COPYFILE_METADATA) != 0) {
        std::cerr << "Warning: Failed to restore metadata from " << backup
                  << " to " << target << ": " << std::strerror(errno) << "\n";

        // Rollback: remove cloned file and restore backup
        if (unlink(target.c_str()) != 0) {
            std::cerr << "Rollback error: failed to remove cloned file " << target
                      << ": " << std::strerror(errno) << "\n";
        }
        if (rename(backup.c_str(), target.c_str()) != 0) {
            std::cerr << "Rollback error: failed to restore original file " << target
                      << " from " << backup << ": " << std::strerror(errno) << "\n";
        }
        return -1;
    }

    // Step 4: Cleanup the backup
    if (unlink(backup.c_str()) != 0) {
        std::cerr << "Warning: Failed to remove backup file " << backup
                  << ": " << std::strerror(errno) << "\n";
        // Not a critical failure, we have a successfully cloned and restored file
    }

    // Success
    return 0;
}

int Fileinfo::static_makeclone(Fileinfo& A, const Fileinfo& B) {
    return A.makeclone(B);
}
#endif
