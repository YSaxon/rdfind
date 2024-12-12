/*
   copyright 2006-2017 Paul Dreik (earlier Paul Sundvall)
   Distributed under GPL v 2.0 or later, at your option.
   See LICENSE for further details.
*/

// pick up project config incl. assert control.
#include "config.h"

// std
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>  //for file writing
#include <iostream> //for std::cerr
#include <ostream>  //for output
#include <string>   //for easier passing of string arguments
#include <thread>   //sleep

// project
#include "Fileinfo.hh" //file container
#include "RdfindDebug.hh"

// class declaration
#include "Rdutil.hh"

#if defined(HAVE_APFS_CLONING)
#include <set>       // for std::set
#include <sys/types.h> // for off_t
#endif

int
Rdutil::printtofile(const std::string& filename) const
{
  // open a file to print to
  std::ofstream f1;
  f1.open(filename.c_str(), std::ios_base::out);
  if (!f1.is_open()) {
    std::cerr << "could not open file \"" << filename << "\"\n";
    return -1;
  }

  // exchange f1 for cout to write to terminal instead of file
  std::ostream& output(f1);

  // This uses "priority" instead of "cmdlineindex". Change this the day
  // a change in output format is allowed (for backwards compatibility).
  output << "# Automatically generated\n";
  output << "# duptype id depth size device inode priority name\n";

  std::vector<Fileinfo>::iterator it;
  for (it = m_list.begin(); it != m_list.end(); ++it) {
    output << Fileinfo::getduptypestring(*it) << " " << it->getidentity() << " "
           << it->depth() << " " << it->size() << " " << it->device() << " "
           << it->inode() << " " << it->get_cmdline_index() << " " << it->name()
           << '\n';
  }
  output << "# end of file\n";
  f1.close();
  return 0;
}

// applies int f(duplicate,const original) on every duplicate.
// if f returns nonzero, something is wrong.
// returns how many times the function was invoked.
template<typename Function>
std::size_t
applyactiononfile(std::vector<Fileinfo>& m_list, Function f)
{

  const auto first = m_list.begin();
  const auto last = m_list.end();
  auto original = last;

  std::size_t ntimesapplied = 0;

  // loop over files
  for (auto it = first; it != last; ++it) {
    switch (it->getduptype()) {
      case Fileinfo::duptype::DUPTYPE_FIRST_OCCURRENCE: {
        original = it;
        assert(original->getidentity() >= 0 &&
               "original file should have positive identity");
      } break;

      case Fileinfo::duptype::DUPTYPE_OUTSIDE_TREE:
        // intentional fallthrough
      case Fileinfo::duptype::DUPTYPE_WITHIN_SAME_TREE: {
        assert(original != last);
        // double check that "it" shall be ~linked to "src"
        assert(it->getidentity() == -original->getidentity() &&
               "it must be connected to src");
        // everything is in order. we may now hardlink/symlink/remove it.
        if (f(*it, *original)) {
          RDDEBUG(__FILE__ ": Failed to apply function f on it.\n");
        } else {
          ++ntimesapplied;
        }
      } break;

      default:
        assert("file with bad duptype at this stage. Programming error!" ==
               nullptr);
    }
  }
  return ntimesapplied;
}

// helper for dryruns
namespace {
template<bool outputBname>
class dryrun_helper
{
public:
  /// @param m1 may not be null
  /// @param m2 may be null
  explicit dryrun_helper(const char* m1, const char* m2 = nullptr)
    : m_m1(m1)
    , m_m2(m2)
  {}

  const char* const m_m1;
  const char* const m_m2;

  /// Mimic the return value of makeHardlinks etc. - pretend success
  int operator()(const Fileinfo& A, const Fileinfo& B) const
  {
    std::cout << "(DRYRUN MODE) " << m_m1 << A.name();
    if (m_m2) {
      std::cout << m_m2;
    }
    if (outputBname) {
      std::cout << B.name();
    }
    std::cout << '\n';

    return 0;
  }
}; // class
} // namespace

std::size_t
Rdutil::deleteduplicates(bool dryrun) const
{
  if (dryrun) {
    const bool outputBname = false;
    dryrun_helper<outputBname> obj("delete ");
    auto ret = applyactiononfile(m_list, obj);
    std::cout.flush();
    return ret;
  } else {
    return applyactiononfile(m_list, &Fileinfo::static_deletefile);
  }
}

std::size_t
Rdutil::makesymlinks(bool dryrun) const
{
  if (dryrun) {
    const bool outputBname = true;
    dryrun_helper<outputBname> obj("symlink ", " to ");
    auto ret = applyactiononfile(m_list, obj);
    std::cout.flush();
    return ret;
  } else {
    return applyactiononfile(m_list, &Fileinfo::static_makesymlink);
  }
}

std::size_t
Rdutil::makehardlinks(bool dryrun) const
{
  if (dryrun) {
    const bool outputBname = true;
    dryrun_helper<outputBname> obj("hardlink ", " to ");
    const auto ret = applyactiononfile(m_list, obj);
    std::cout.flush();
    return ret;
  } else {
    return applyactiononfile(m_list, &Fileinfo::static_makehardlink);
  }
}

#if defined(HAVE_APFS_CLONING)
std::size_t
Rdutil::makeclones(bool dryrun) const
{
    if (dryrun) {
        const bool outputBname = true;
        dryrun_helper<outputBname> obj("clone ", " from ");
        auto ret = applyactiononfile(m_list, obj);
        std::cout.flush();
        return ret;
    }

    bool warned_about_non_apfs = false;
    std::size_t already_cloned = 0;

    // Wrap file operation in lambda to handle APFS checks and existing clones
    auto clone_op = [&](Fileinfo& A, const Fileinfo& B) -> int {
        if (!A.is_on_apfs() || !B.is_on_apfs()) {
            if (!warned_about_non_apfs) {
                std::cerr << "Warning: some files are not on APFS filesystems. These will be skipped.\n";
                warned_about_non_apfs = true;
            }
            return 0; // Skip but don't count as error
        }

        if (A.is_clone_of(B)) {
          
            ++already_cloned;
            return 0; // Skip but don't count as error
        }

        return Fileinfo::static_makeclone(A, B);
    };

    auto cloned = applyactiononfile(m_list, clone_op);

    if (already_cloned > 0) {
        std::cout << "Skipped " << already_cloned << " files that were already clones." << std::endl;
    }

    return cloned - already_cloned;
}
  #endif

// mark files with a unique number
void
Rdutil::markitems()
{
  std::int64_t fileno = 1;
  for (auto& file : m_list) {
    file.setidentity(fileno++);
  }
}

namespace {
bool
cmpDeviceInode(const Fileinfo& a, const Fileinfo& b)
{
  return std::make_tuple(a.device(), a.inode()) <
         std::make_tuple(b.device(), b.inode());
}
// compares rank as described in RANKING on man page.
bool
cmpRank(const Fileinfo& a, const Fileinfo& b)
{
  return std::make_tuple(a.get_cmdline_index(), a.depth(), a.getidentity()) <
         std::make_tuple(b.get_cmdline_index(), b.depth(), b.getidentity());
}
bool
cmpDepthName(const Fileinfo& a, const Fileinfo& b)
{
  // inefficient, make it a reference.
  return std::make_tuple(a.depth(), a.name()) <
         std::make_tuple(b.depth(), b.name());
}
// compares buffers
bool
cmpBuffers(const Fileinfo& a, const Fileinfo& b)
{
  return std::memcmp(a.getbyteptr(), b.getbyteptr(), a.getbuffersize()) < 0;
}

#if !defined(NDEBUG)
bool
hasEqualBuffers(const Fileinfo& a, const Fileinfo& b)
{
  return std::memcmp(a.getbyteptr(), b.getbyteptr(), a.getbuffersize()) == 0;
}
#endif

// compares file size
bool
cmpSize(const Fileinfo& a, const Fileinfo& b)
{
  return a.size() < b.size();
}
bool
cmpSizeThenBuffer(const Fileinfo& a, const Fileinfo& b)
{
  return (a.size() < b.size()) || (a.size() == b.size() && cmpBuffers(a, b));
}

/**
 * goes through first to last, finds ranges of equal elements (determined by
 * cmp) and invokes callback on each subrange.
 * @param first
 * @param last
 * @param cmp
 * @param callback invoked as callback(subrangefirst,subrangelast)
 */
template<class Iterator, class Cmp, class Callback>
void
apply_on_range(Iterator first, Iterator last, Cmp cmp, Callback callback)
{
  assert(std::is_sorted(first, last, cmp));

  // switch between linear search and logarithmic. the linear should be good
  // because most of the time, we search very few adjacent elements.
  // the difference seem to be within measurement noise.
  const bool linearsearch = false;

  if (linearsearch) {
    while (first != last) {
      auto sublast = first + 1;
      while (sublast != last && !cmp(*first, *sublast)) {
        ++sublast;
      }
      // a duplicate range with respect to cmp
      callback(first, sublast);

      // keep searching.
      first = sublast;
    }
  } else {
    while (first != last) {
      auto p = std::equal_range(first, last, *first, cmp);
      // p.first will point to first. p.second will point to first+1 if no
      // duplicate is found
      assert(p.first == first);

      // a duplicate range with respect to cmp
      callback(p.first, p.second);

      // keep searching.
      first = p.second;
    }
  }
}
} // namespace
int
Rdutil::sortOnDeviceAndInode()
{

  std::sort(m_list.begin(), m_list.end(), cmpDeviceInode);
  return 0;
}

void
Rdutil::sort_on_depth_and_name(std::size_t index_of_first)
{
  assert(index_of_first <= m_list.size());

  auto it = std::begin(m_list) + static_cast<std::ptrdiff_t>(index_of_first);
  std::sort(it, std::end(m_list), cmpDepthName);
}

std::size_t
Rdutil::removeIdenticalInodes()
{
  // sort list on device and inode.
  auto cmp = cmpDeviceInode;
  std::sort(m_list.begin(), m_list.end(), cmp);

  // loop over ranges of adjacent elements
  using Iterator = decltype(m_list.begin());
  apply_on_range(
    m_list.begin(), m_list.end(), cmp, [](Iterator first, Iterator last) {
      // let the highest-ranking element not be deleted. do this in order, to be
      // cache friendly.
      auto best = std::min_element(first, last, cmpRank);
      std::for_each(first, best, [](Fileinfo& f) { f.setdeleteflag(true); });
      best->setdeleteflag(false);
      std::for_each(best + 1, last, [](Fileinfo& f) { f.setdeleteflag(true); });
    });
  return cleanup();
}

std::size_t
Rdutil::removeUniqueSizes()
{
  // sort list on size
  auto cmp = cmpSize;
  std::sort(m_list.begin(), m_list.end(), cmp);

  // loop over ranges of adjacent elements
  using Iterator = decltype(m_list.begin());
  apply_on_range(
    m_list.begin(), m_list.end(), cmp, [](Iterator first, Iterator last) {
      if (first + 1 == last) {
        // single element. remove it!
        first->setdeleteflag(true);
      } else {
        // multiple elements. keep them!
        std::for_each(first, last, [](Fileinfo& f) { f.setdeleteflag(false); });
      }
    });
  return cleanup();
}

std::size_t
Rdutil::removeUniqSizeAndBuffer()
{
  // sort list on size
  const auto cmp = cmpSize;
  std::sort(m_list.begin(), m_list.end(), cmp);

  const auto bufcmp = cmpBuffers;

  // loop over ranges of adjacent elements
  using Iterator = decltype(m_list.begin());
  apply_on_range(
    m_list.begin(), m_list.end(), cmp, [&](Iterator first, Iterator last) {
      // all sizes are equal in [first,last) - sort on buffer content.
      std::sort(first, last, bufcmp);

      // on this set of buffers, find those which are unique
      apply_on_range(
        first, last, bufcmp, [](Iterator firstbuf, Iterator lastbuf) {
          if (firstbuf + 1 == lastbuf) {
            // we have a unique buffer!
            firstbuf->setdeleteflag(true);
          } else {
            std::for_each(
              firstbuf, lastbuf, [](Fileinfo& f) { f.setdeleteflag(false); });
          }
        });
    });

  return cleanup();
}

void
Rdutil::markduplicates()
{
  const auto cmp = cmpSizeThenBuffer;
  assert(std::is_sorted(m_list.begin(), m_list.end(), cmp));

  // loop over ranges of adjacent elements
  using Iterator = decltype(m_list.begin());
  apply_on_range(
    m_list.begin(),
    m_list.end(),
    cmp,
    [](const Iterator first, const Iterator last) {
      // size and buffer are equal in  [first,last) - all are duplicates!
      assert(std::distance(first, last) >= 2);

      // the one with the lowest rank is the original
      auto orig = std::min_element(first, last, cmpRank);
      assert(orig != last);
      // place it first, so later stages will find the original first.
      std::iter_swap(first, orig);
      orig = first;

      // make sure they are all duplicates
      assert(last == find_if_not(first, last, [orig](const Fileinfo& a) {
               return orig->size() == a.size() && hasEqualBuffers(*orig, a);
             }));

      // mark the files with the appropriate tag.
      auto marker = [orig](Fileinfo& elem) {
        elem.setidentity(-orig->getidentity());
        if (elem.get_cmdline_index() == orig->get_cmdline_index()) {
          elem.setduptype(Fileinfo::duptype::DUPTYPE_WITHIN_SAME_TREE);
        } else {
          elem.setduptype(Fileinfo::duptype::DUPTYPE_OUTSIDE_TREE);
        }
      };
      orig->setduptype(Fileinfo::duptype::DUPTYPE_FIRST_OCCURRENCE);
      std::for_each(first + 1, last, marker);
      assert(first->getduptype() ==
             Fileinfo::duptype::DUPTYPE_FIRST_OCCURRENCE);
    });
}

std::size_t
Rdutil::cleanup()
{
  const auto size_before = m_list.size();
  auto it = std::remove_if(m_list.begin(), m_list.end(), [](const Fileinfo& A) {
    return A.deleteflag();
  });

  m_list.erase(it, m_list.end());

  const auto size_after = m_list.size();

  return size_before - size_after;
}
#if 0
std::size_t
Rdutil::remove_small_files(Fileinfo::filesizetype minsize)
{
  const auto size_before = m_list.size();
  const auto begin = m_list.begin();
  const auto end = m_list.end();
  decltype(m_list.begin()) it;
  if (minsize == 0) {
    it =
      std::remove_if(begin, end, [](const Fileinfo& A) { return A.isempty(); });
  } else {
    it = std::remove_if(begin, end, [=](const Fileinfo& A) {
      return A.is_smaller_than(minsize);
    });
  }
  m_list.erase(it, end);
  return size_before - m_list.size();
}
#endif

Fileinfo::filesizetype
Rdutil::totalsizeinbytes(int opmode) const
{
  assert(opmode == 0 || opmode == 1
  #if defined(HAVE_APFS_CLONING)
         || opmode == 2
  #endif
  );

  Fileinfo::filesizetype totalsize = 0;
  if (opmode == 0) {
    for (const auto& elem : m_list) {
      totalsize += elem.size();
    }
  } else if (opmode == 1) {
    for (const auto& elem : m_list) {
      if (elem.getduptype() == Fileinfo::duptype::DUPTYPE_FIRST_OCCURRENCE) {
        totalsize += elem.size();
      }
    }
  }
  #if defined(HAVE_APFS_CLONING)
  else if (opmode == 2) //total size on disk taking existing cow copies into account
  {
    std::set<off_t> unique_blocks;
    for (const auto& elem : m_list) {
        off_t cow_id = elem.get_cow_id();
        if (cow_id == -1 || unique_blocks.insert(cow_id).second) {
            totalsize += elem.size();
        }
    }
  }
  #endif

  return totalsize;
}
namespace littlehelper {
// helper to make "size" into a more readable form.
int
calcrange(Fileinfo::filesizetype& size)
{
  int range = 0;
  Fileinfo::filesizetype tmp = 0;
  while (size > 1024) {
    tmp = size >> 9;
    size = (tmp >> 1);
    ++range;
  }

  // round up if necessary
  if (tmp & 0x1) {
    ++size;
  }

  return range;
}

// source of capitalization rules etc:
// https://en.wikipedia.org/wiki/Binary_prefix
std::string
byteprefix(int range)
{
  switch (range) {
    case 0:
      return "B";
    case 1:
      return "KiB";
    case 2:
      return "MiB";
    case 3:
      return "GiB";
    case 4:
      return "TiB"; // Tebibyte
    case 5:
      return "PiB"; // Pebibyte
    case 6:
      return "EiB"; // Exbibyte
    default:
      return "!way too much!";
  }
}
} // namespace littlehelper

std::ostream&
Rdutil::totalsize(std::ostream& out, int opmode) const
{
  auto size = totalsizeinbytes(opmode);
  const int range = littlehelper::calcrange(size);
  out << size << " " << littlehelper::byteprefix(range);
  return out;
}

std::ostream&
Rdutil::saveablespace(std::ostream& out) const
{
  auto size = totalsizeinbytes(0) - totalsizeinbytes(1);
  int range = littlehelper::calcrange(size);
  out << size << " " << littlehelper::byteprefix(range);
  return out;
}

std::ostream&
Rdutil::cloneaware_saveablespace(std::ostream& out) const
{
  auto size = totalsizeinbytes(2) - totalsizeinbytes(1);
  int range = littlehelper::calcrange(size);
  out << size << " " << littlehelper::byteprefix(range);
  return out;
}

int
Rdutil::fillwithbytes(enum Fileinfo::readtobuffermode type,
                      enum Fileinfo::readtobuffermode lasttype,
                      const long nsecsleep)
{
  // first sort on inode (to read efficiently from the hard drive)
  sortOnDeviceAndInode();

  const auto duration = std::chrono::nanoseconds{ nsecsleep };

  for (auto& elem : m_list) {
    elem.fillwithbytes(type, lasttype);
    if (nsecsleep > 0) {
      std::this_thread::sleep_for(duration);
    }
  }
  return 0;
}
