/**
 * This file is part of the CernVM File System.
 */

#include <gtest/gtest.h>
#ifdef __APPLE__
  #include <sys/sysctl.h>
#endif
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <fstream>  // TODO(jblomer): remove me
#include <map>
#include <sstream>  // TODO(jblomer): remove me

#include "crypto/hash.h"
#include "manifest.h"
#include "testutil.h"
#include "util/fs_traversal.h"


#ifndef __APPLE__
static void SkipWhitespace(std::istringstream *iss) {
  while (iss->good()) {
    const char next = iss->peek();
    if (next != ' ' && next != '\t') {
      break;
    }
    iss->get();
  }
}
#endif


pid_t GetParentPid(const pid_t pid) {
  pid_t parent_pid = 0;

#ifdef __APPLE__
  int mib[4];
  size_t len;
  struct kinfo_proc kp;

  len = 4;
  sysctlnametomib("kern.proc.pid", mib, &len);

  mib[3] = pid;
  len = sizeof(kp);
  if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0) {
    parent_pid = kp.kp_eproc.e_ppid;
  }
#else
  static const std::string ppid_label = "PPid:";

  std::stringstream proc_status_path;
  proc_status_path << "/proc/" << pid << "/status";

  std::ifstream proc_status(proc_status_path.str().c_str());

  std::string line;
  while (std::getline(proc_status, line)) {
    if (line.compare(0, ppid_label.size(), ppid_label) == 0) {
      const std::string s_ppid = line.substr(ppid_label.size());
      std::istringstream iss_ppid(s_ppid);
      SkipWhitespace(&iss_ppid);
      int i_ppid = 0; iss_ppid >> i_ppid;
      if (i_ppid > 0) {
        parent_pid = static_cast<pid_t>(i_ppid);
      }
      break;
    }
  }
#endif

  return parent_pid;
}

std::string GetProcessname(const pid_t pid) {
#ifdef __APPLE__
  std::cerr << "Implementation missing to get process name by pid on MAC\n";
  return "NO VALID PROCESSNAME";
#else
  static const std::string label = "Name:";

  std::stringstream proc_status_path;
  proc_status_path << "/proc/" << pid << "/status";

  std::ifstream proc_status(proc_status_path.str().c_str());

  std::string line;
  while (std::getline(proc_status, line)) {
    if (line.compare(0, label.size(), label) == 0) {
      const std::string line_without_label = line.substr(label.size());

      return Trim(line_without_label);
    }
  }
  return "NO VALID PROCESSNAME";
#endif
}


class ShowOpenFilesHelper {
 public:
  void ShowSymlink(const string &parent_path, const string &name) {
    char buf[1024];
    string full_path = parent_path + "/" + name;
    int size = readlink(full_path.c_str(), buf, 1024);
    file_list += (size < 0)
                  ? ("failed to read fd " + name + "\n")
                  : string(buf, size) + "\n";
  }

  string file_list;
};


string ShowOpenFiles() {
#ifdef __APPLE__
  return "listing open files not supported on OS X";
#else
  ShowOpenFilesHelper open_files_helper;
  FileSystemTraversal<ShowOpenFilesHelper>
    traversal(&open_files_helper, "", false);
  traversal.fn_new_symlink = &ShowOpenFilesHelper::ShowSymlink;
  traversal.Recurse("/proc/self/fd");
  return open_files_helper.file_list;
#endif
}


class CountOpenFilesHelper {
 public:
  CountOpenFilesHelper() : count(0) {}

  void CountSymlink(const string & /* parent_path */, const string & /* name */)
  {
    count++;
  }

  unsigned count;
};

unsigned GetNoUsedFds() {
  // Syslog file descriptor could still be open
  closelog();

#ifdef __APPLE__
  unsigned result = 0;
  int max_fd = getdtablesize();
  assert(max_fd >= 0);
  for (unsigned fd = 0; fd < unsigned(max_fd); ++fd) {
    int retval = fcntl(fd, F_GETFD, 0);
    if (retval != -1)
      result++;
  }
  return result;
#else
  CountOpenFilesHelper count_files_helper;
  FileSystemTraversal<CountOpenFilesHelper>
    traversal(&count_files_helper, "", false);
  traversal.fn_new_symlink = &CountOpenFilesHelper::CountSymlink;
  traversal.Recurse("/proc/self/fd");
  return count_files_helper.count;
#endif
}


/**
 * Traverses the $PATH environment variable to find the absolute path of a given
 * program name. Pretty much what `which` in bash would do.
 *
 * Inspired by a solution to this bulletin board question:
 *  http://www.linuxquestions.org/questions/programming-9/g
 *         et-full-path-of-a-command-in-c-117965/#post611028
 *
 * @param exe_name  the name of the program to search the $PATH for
 * @return          absolute path to the program or empty string if not found
 */
std::string GetExecutablePath(const std::string &exe_name) {
  std::string result;

  if (exe_name.empty() || exe_name.find('/') != std::string::npos) {
    return result;
  }

  const char *searchpath = getenv("PATH");
  if (NULL == searchpath) {
    return result;
  }

  const std::vector<std::string> paths = SplitString(searchpath, ':');
        std::vector<std::string>::const_iterator i    = paths.begin();
  const std::vector<std::string>::const_iterator iend = paths.end();
  for (; i != iend; ++i) {
    const std::string candidate_path = *i + "/" + exe_name;
    char real_path[PATH_MAX];

    const char *rpret = realpath(candidate_path.c_str(), real_path);
    if (NULL == rpret) {
      continue;
    }

    platform_stat64 statinfo;
    const int res = platform_stat(real_path, &statinfo);

    if (res < 0) {
      continue;
    }

    if (!S_ISREG(statinfo.st_mode)) {
      break;
    }

    if (statinfo.st_mode & S_IXUSR ||
        statinfo.st_mode & S_IXGRP ||
        statinfo.st_mode & S_IXOTH) {
      result = real_path;
      break;
    }
  }

  return result;
}

time_t t(const int day, const int month, const int year) {
  struct tm time_descriptor;

  time_descriptor.tm_hour  = 0;
  time_descriptor.tm_min   = 0;
  time_descriptor.tm_sec   = 0;
  time_descriptor.tm_mday  = day;
  time_descriptor.tm_mon   = month - 1;
  time_descriptor.tm_year  = year - 1900;
  time_descriptor.tm_isdst = 0;

  const time_t result = mktime(&time_descriptor);
  assert(result >= 0);
  return result;
}

shash::Any h(const std::string &hash, const shash::Suffix suffix) {
  return shash::Any(shash::kSha1, shash::HexPtr(hash), suffix);
}

namespace catalog {

DirectoryEntry DirectoryEntryTestFactory::RegularFile(const string &name,
                                                      unsigned size,
                                                      shash::Any hash) {
  DirectoryEntry dirent;
  dirent.mode_ = 33188;
  dirent.name_ = NameString(name);
  dirent.checksum_ = hash;
  dirent.size_ = size;
  return dirent;
}


DirectoryEntry DirectoryEntryTestFactory::ExternalFile() {
  DirectoryEntry dirent;
  dirent.mode_ = 33188;
  dirent.is_external_file_ = true;
  return dirent;
}

DirectoryEntry DirectoryEntryTestFactory::SpecialFile() {
  DirectoryEntry dirent;
  dirent.mode_ = 4516;  // FIFO
  return dirent;
}

DirectoryEntry DirectoryEntryTestFactory::Directory(
    const string &name,
    unsigned size,
    shash::Any hash,
    bool is_nested_catalog_mountpoint)
{
  DirectoryEntry dirent;
  dirent.mode_ = 16893;
  dirent.name_ = NameString(name);
  dirent.checksum_ = hash;
  dirent.size_ = size;
  dirent.is_nested_catalog_mountpoint_ = is_nested_catalog_mountpoint;
  return dirent;
}


DirectoryEntry DirectoryEntryTestFactory::Symlink(const string &name,
                                                  unsigned size,
                                                  const string &symlink_path) {
  DirectoryEntry dirent;
  dirent.mode_ = 41471;
  dirent.name_ = NameString(name);
  dirent.size_ = size;
  dirent.symlink_ = LinkString(symlink_path);
  return dirent;
}


DirectoryEntry DirectoryEntryTestFactory::ChunkedFile(shash::Any content_hash) {
  DirectoryEntry dirent;
  dirent.mode_ = 33188;
  dirent.is_chunked_file_ = true;
  dirent.checksum_ = content_hash;
  return dirent;
}

catalog::DirectoryEntry catalog::DirectoryEntryTestFactory::Make(
    const Metadata& metadata) {
  DirectoryEntry dirent;
  dirent.name_       = NameString(metadata.name);
  dirent.mode_       = metadata.mode;
  dirent.uid_        = metadata.uid;
  dirent.gid_        = metadata.gid;
  dirent.size_       = metadata.size;
  dirent.mtime_      = metadata.mtime;
  dirent.symlink_    = LinkString(metadata.symlink);
  dirent.linkcount_  = metadata.linkcount;
  dirent.has_xattrs_ = metadata.has_xattrs;
  dirent.checksum_   = metadata.checksum;
  dirent.is_hidden_  = metadata.is_hidden;
  return dirent;
}

}  // namespace catalog


//------------------------------------------------------------------------------



atomic_int32 MockCatalog::instances;

const std::string MockCatalog::rhs =
  "f9d87ae2cc46be52b324335ff05fae4c1a7c4dd4";
const shash::Any MockCatalog::root_hash =
  shash::Any(shash::kSha1, shash::HexPtr(MockCatalog::rhs),
             shash::kSuffixCatalog);

void MockCatalog::ResetGlobalState() {
  atomic_init32(&MockCatalog::instances);
}

MockCatalog* MockCatalog::AttachFreely(const std::string  &root_path,
                                       const std::string  &file,
                                       const shash::Any   &catalog_hash,
                                             MockCatalog  *parent,
                                       const bool          is_not_root) {
  MockCatalog *catalog = MockCatalog::Get(catalog_hash);

  if (catalog == NULL) {
    return NULL;
  }

  assert(catalog->IsRoot() || is_not_root);
  MockCatalog *new_catalog = catalog->Clone();
  new_catalog->set_parent(parent);
  return new_catalog;
}

void MockCatalog::RemoveChild(MockCatalog *child) {
  std::vector<NestedCatalog>::iterator iter = active_children_.begin();
  while (iter != active_children_.end()) {
    if (iter->hash == child->hash()) {
      active_children_.erase(iter);
      return;
    }
    iter++;
  }
}

MockCatalog* MockCatalog::FindSubtree(const PathString &path) {
  for (unsigned i = 0; i < active_children_.size(); ++i) {
    if (active_children_[i].mountpoint == path)
      return active_children_[i].child;
  }
  return NULL;
}


bool MockCatalog::FindNested(
  const PathString &mountpoint,
  shash::Any *hash,
  uint64_t *size) const
{
  for (unsigned i = 0; i < children_.size(); ++i) {
    if (children_[i].mountpoint == mountpoint) {
      *hash = children_[i].hash;
      *size = children_[i].size;
      return true;
    }
  }
  return false;
}


bool MockCatalog::LookupPath(const PathString &path,
                catalog::DirectoryEntry *dirent) const {
  shash::Md5 md5_path(path.GetChars(), path.GetLength());
  for (unsigned i = 0; i < files_.size(); ++i) {
    if (files_[i].path_hash == md5_path) {
      *dirent = files_[i].ToDirectoryEntry();
      return true;
    }
  }
  return false;
}

bool MockCatalog::ListingPath(const PathString &path,
                 catalog::DirectoryEntryList *listing,
                 const bool /* expand_symlink */) const {
  unsigned initial_size = listing->size();
  shash::Md5 path_hash(path.GetChars(), path.GetLength());
  for (unsigned i = 0; i < files_.size(); ++i) {
    if (files_[i].parent_hash == path_hash && files_[i].name != "")
      listing->push_back(files_[i].ToDirectoryEntry());
  }
  return listing->size() > initial_size;
}

void MockCatalog::RegisterNestedCatalog(MockCatalog *child) {
  NestedCatalog nested;
  nested.mountpoint = PathString(child->root_path());
  nested.hash = child->hash();
  nested.child = child;
  nested.size  = child->catalog_size();
  children_.push_back(nested);

  // update the directory entries in both catalogs
  string path = child->root_path();
  File *mountpoint = FindFile(path);
  if (mountpoint != NULL) {
    mountpoint->is_nested_catalog_mountpoint = true;
  }
  File *child_mountpoint = child->FindFile(path);
  if (child_mountpoint != NULL) {
    child_mountpoint->is_nested_catalog_mountpoint = true;
  }
}

void MockCatalog::AddChild(MockCatalog *child) {
  NestedCatalog nested;
  nested.mountpoint = PathString(child->root_path());
  nested.hash = child->hash();
  nested.child = child;
  nested.size = child->catalog_size();
  active_children_.push_back(nested);
}

void MockCatalog::AddFile(const shash::Any   &content_hash,
                          const size_t        file_size,
                          const string        &parent_path,
                          const string        &name)
{
  MockCatalog::File f(content_hash, file_size, parent_path, name);
  files_.push_back(f);
}

void MockCatalog::AddChunk(const shash::Any  &chunk_content_hash,
                           const size_t       chunk_size) {
  MockCatalog::Chunk c;
  c.hash = chunk_content_hash;
  c.size = chunk_size;
  chunks_.push_back(c);
}

template <class T>
struct HashExtractor {
  const shash::Any& operator() (const T &object) const {
    return object.hash;
  }
};

const MockCatalog::HashVector& MockCatalog::GetReferencedObjects() const {
  if (referenced_objects_.empty()) {
    const size_t num_objs = files_.size() + chunks_.size();
    referenced_objects_.resize(num_objs);
    HashVector::iterator i = referenced_objects_.begin();

    i = std::transform(files_.begin(), files_.end(),
                       i, HashExtractor<File>());
    i = std::transform(chunks_.begin(), chunks_.end(),
                       i, HashExtractor<Chunk>());
  }

  return referenced_objects_;
}


//------------------------------------------------------------------------------


MockCatalog* catalog::MockCatalogManager::CreateCatalog(
                               const PathString  &mountpoint,
                               const shash::Any  &catalog_hash,
                               MockCatalog *parent_catalog)
{
  map<PathString, MockCatalog*>::iterator it = catalog_map_.find(mountpoint);
  if (it != catalog_map_.end()) {
    return it->second;
  }
  bool is_root = parent_catalog == NULL;
  return new MockCatalog(mountpoint.ToString(), catalog_hash, 4096, 1,
                         0, is_root, parent_catalog, NULL);
}

catalog::LoadError catalog::MockCatalogManager::LoadCatalog(
                                                  const PathString &mountpoint,
                                                  const shash::Any &hash,
                                                  string  *catalog_path,
                                                  shash::Any *catalog_hash)
{
  map<PathString, MockCatalog*>::iterator it = catalog_map_.find(mountpoint);
  if (it != catalog_map_.end() && catalog_hash != NULL) {
    MockCatalog *catalog = it->second;
    *catalog_hash = catalog->hash();
  } else {
    MockCatalog *catalog = new MockCatalog(mountpoint.ToString(),
                                           hash, 4096, 1, 0,
                                           true, NULL, NULL);
    catalog_map_[mountpoint] = catalog;
  }
  return kLoadNew;
}


//------------------------------------------------------------------------------


MockObjectFetcher::Failures
MockObjectFetcher::FetchManifest(manifest::Manifest** manifest) {
  const uint64_t    catalog_size = 0;
  const std::string root_path    = "";
  *manifest = new manifest::Manifest(
      MockCatalog::root_hash,
      catalog_size,
      root_path);
  (*manifest)->set_history(MockHistory::root_hash);
  return MockObjectFetcher::kFailOk;
}

MockObjectFetcher::Failures
MockObjectFetcher::Fetch(const shash::Any   &object_hash,
                               std::string  *file_path) {
  assert(file_path != NULL);
  *file_path = object_hash.ToString();
  if (!ObjectExists(object_hash)) {
    return MockObjectFetcher::kFailNotFound;
  }
  return MockObjectFetcher::kFailOk;
}

MockObjectFetcher::Failures
MockObjectFetcher::Fetch(const std::string &relative_path,
                         const bool         decompress,
                         const bool         nocache,
                         std::string *file_path) {
  *file_path = relative_path;
  if (!PathExists(relative_path)) {
    return MockObjectFetcher::kFailNotFound;
  }
  return MockObjectFetcher::kFailOk;
}

bool MockObjectFetcher::ObjectExists(const shash::Any &object_hash) const {
  return MockCatalog::Exists(object_hash) ||
         MockHistory::Exists(object_hash);
}

bool MockObjectFetcher::PathExists(const std::string &path) const {
  return MockReflog::Exists(path);
}


//------------------------------------------------------------------------------


unsigned int MockHistory::instances = 0;
const std::string MockHistory::rhs =
  "b46091c745a1ffef707dd7eabec852fb8679cf28";
const shash::Any  MockHistory::root_hash =
  shash::Any(shash::kSha1, shash::HexPtr(MockHistory::rhs),
             shash::kSuffixHistory);

void MockHistory::ResetGlobalState() {
  MockHistory::instances = 0;
}


MockHistory* MockHistory::Open(const std::string &path) {
  const shash::Any history_hash(shash::MkFromHexPtr(shash::HexPtr(path),
                                                    shash::kSuffixHistory));
  MockHistory *history = MockHistory::Get(history_hash);
  return (history != NULL) ? history->Clone() : NULL;
}


MockHistory::MockHistory(const bool          writable,
                         const std::string  &fqrn)
        : writable_(writable)
        , owns_database_file_(false) {
  set_fqrn(fqrn);
  branches_[""] = History::Branch("", "", 0);
  ++MockHistory::instances;
}


MockHistory::MockHistory(const MockHistory &other)
  : tags_(other.tags_)
  , recycle_bin_(other.recycle_bin_)
  , writable_(other.writable_)
  , previous_revision_(other.previous_revision_)
  , owns_database_file_(false)
{
  set_fqrn(other.fqrn());
  ++MockHistory::instances;
}


MockHistory::~MockHistory() {
  --MockHistory::instances;
}


MockHistory* MockHistory::Clone(const bool writable) const {
  MockHistory *new_copy = new MockHistory(*this);
  new_copy->set_writable(writable);
  return new_copy;
}


void MockHistory::GetTags(std::vector<Tag> *tags) const {
  tags->clear();
  tags->resize(tags_.size());
  std::transform(tags_.begin(), tags_.end(),
                 tags->begin(), MockHistory::get_tag);
}


bool MockHistory::Insert(const Tag &tag) {
  if (Exists(tag.name)) {
    return false;
  }
  bool found_branch = false;
  for (BranchMap::const_iterator i = branches_.begin();
       i != branches_.end(); ++i)
  {
    if (i->first == tag.branch) {
      found_branch = true;
      break;
    }
  }
  if (!found_branch)
    return false;

  tags_[tag.name] = tag;
  return true;
}

bool MockHistory::Remove(const std::string &name) {
  Tag tag;
  if (!GetByName(name, &tag)) {
    return true;
  }

  return tags_.erase(name) == 1;
}

bool MockHistory::Exists(const std::string &name) const {
  return tags_.find(name) != tags_.end();
}

bool MockHistory::GetByName(const std::string &name, Tag *tag) const {
  TagMap::const_iterator t = tags_.find(name);
  if (t == tags_.end()) {
    return false;
  }
  *tag = t->second;
  return true;
}

bool MockHistory::GetByDate(const time_t timestamp, Tag *tag) const {
  typedef std::vector<Tag> Tags;
  Tags tags;
  if (!List(&tags)) {
    return false;
  }

  DateSmallerThan pred(timestamp);
  const Tags::const_iterator t = std::find_if(tags.begin(), tags.end(), pred);
  if (t == tags.end()) {
    return false;
  }

  *tag = *t;
  return true;
}

bool MockHistory::List(std::vector<Tag> *tags) const {
  GetTags(tags);
  std::sort(tags->rbegin(), tags->rend());
  return true;
}

bool MockHistory::GetBranchHead(const string &branch_name, Tag *tag) const {
  std::vector<Tag> all_tags;
  GetTags(&all_tags);
  int max_rev = -1;
  for (unsigned i = 0; i < all_tags.size(); ++i) {
    if (all_tags[i].branch != branch_name)
      continue;
    if (static_cast<int>(all_tags[i].revision) > max_rev) {
      *tag = all_tags[i];
      max_rev = all_tags[i].revision;
    }
  }
  return max_rev > 0;
}

bool MockHistory::InsertBranch(const Branch &branch) {
  bool found_parent = false;
  for (BranchMap::const_iterator i = branches_.begin();
       i != branches_.end(); ++i)
  {
    if (i->first == branch.branch)
      return false;
    if (i->first == branch.parent)
      found_parent = true;
  }
  if (!found_parent)
    return false;
  branches_[branch.branch] = branch;
  return true;
}

bool MockHistory::PruneBranches() {
  return false;
}

bool MockHistory::ListBranches(vector<Branch> *branches) const {
  for (BranchMap::const_iterator i = branches_.begin();
       i != branches_.end(); ++i)
  {
    branches->push_back(Branch(
      i->first, i->second.parent, i->second.initial_revision));
  }
  return true;
}

bool MockHistory::ExistsBranch(const std::string &branch_name) const {
  for (BranchMap::const_iterator i = branches_.begin();
       i != branches_.end(); ++i)
  {
    if (i->first == branch_name)
      return true;
  }
  return false;
}

bool MockHistory::ListRecycleBin(std::vector<shash::Any> *hashes) const {
  hashes->clear();
  hashes->insert(hashes->end(), recycle_bin_.begin(), recycle_bin_.end());
  return true;
}

bool MockHistory::EmptyRecycleBin() {
  recycle_bin_.clear();
  return true;
}

bool MockHistory::Rollback(const Tag &updated_target_tag) {
  std::vector<Tag> affected_tags;
  if (!ListTagsAffectedByRollback(updated_target_tag.name, &affected_tags)) {
    return false;
  }

  TagRemover remover(this);
  std::for_each(affected_tags.begin(), affected_tags.end(), remover);

  return Insert(updated_target_tag);
}

bool MockHistory:: ListTagsAffectedByRollback(const std::string  &tag_name,
                                              std::vector<Tag>   *tags) const {
  History::Tag target_tag;
  if (!GetByName(tag_name, &target_tag)) {
    return false;
  }

  GetTags(tags);

  // TODO(rmeusel): C++11 use std::copy_if
  RollbackPredicate pred(target_tag, true /* inverse */);
  std::vector<Tag>::iterator last = std::remove_copy_if(tags->begin(),
                                                        tags->end(),
                                                        tags->begin(), pred);
  tags->erase(last, tags->end());
  std::sort(tags->rbegin(), tags->rend());

  return true;
}

bool MockHistory::GetHashes(std::vector<shash::Any> *hashes) const {
  // extract tags from TagMap
  std::vector<Tag> tags;
  GetTags(&tags);

  // find hash duplicates
  std::sort(tags.begin(), tags.end(), MockHistory::gt_hashes);
  std::vector<Tag>::iterator last = std::unique(tags.begin(), tags.end(),
                                                MockHistory::eq_hashes);
  tags.erase(last, tags.end());
  std::sort(tags.rbegin(), tags.rend());

  // extract hashes from deduplicated vector
  hashes->clear();
  hashes->resize(tags.size());
  std::transform(tags.rbegin(), tags.rend(),
                 hashes->begin(), MockHistory::get_hash);
  return true;
}


//------------------------------------------------------------------------------


MockReflog* MockReflog::Open(const std::string &path) {
  MockReflog* reflog = MockReflog::Get(path);
  if (NULL == reflog) {
    return NULL;
  }
  return reflog->Clone();
}

MockReflog* MockReflog::Create(const std::string &path,
                               const std::string &repo_name) {
  MockReflog *reflog = new MockReflog(repo_name);
  MockReflog::RegisterPath(path, reflog);
  return reflog;
}


void MockReflog::HashDatabase(
  const std::string &database_path,
  shash::Any *hash_reflog)
{
  HashString("", hash_reflog);
}


MockReflog* MockReflog::Clone() const {
  MockReflog *new_reflog = new MockReflog(*this);
  return new_reflog;
}

MockReflog::MockReflog(const std::string fqrn)
  : owns_database_file_(false)
  , fqrn_(fqrn) {}

bool MockReflog::AddCertificate(const shash::Any &certificate) {
  references_[certificate] = time(NULL);
  return true;
}

bool MockReflog::AddCatalog(const shash::Any &catalog) {
  references_[catalog] = time(NULL);
  return true;
}

bool MockReflog::AddHistory(const shash::Any &history) {
  references_[history] = time(NULL);
  return true;
}

bool MockReflog::AddMetainfo(const shash::Any &metainfo) {
  references_[metainfo] = time(NULL);
  return true;
}

bool MockReflog::List(
  SqlReflog::ReferenceType type,
  std::vector<shash::Any> *hashes) const
{
  return ListOlderThan(type, static_cast<uint64_t>(-1), hashes);
}

bool MockReflog::ListOlderThan(
  SqlReflog::ReferenceType type,
  uint64_t timestamp,
  std::vector<shash::Any> *hashes) const
{
  hashes->clear();
  for (map<shash::Any, uint64_t>::const_iterator i = references_.begin(),
       i_end = references_.end(); i != i_end; ++i)
  {
    if ((i->first.suffix == SqlReflog::ToSuffix(type)) &&
        (i->second < timestamp))
    {
      hashes->push_back(i->first);
    }
  }
  std::sort(hashes->begin(), hashes->end());

  return true;
}

bool MockReflog::GetCatalogTimestamp(
  const shash::Any &catalog,
  uint64_t *timestamp)
{
  std::map<shash::Any, uint64_t>::const_iterator iter =
    references_.find(catalog);
  if (iter == references_.end())
    return false;
  *timestamp = iter->second;
  return true;
}


bool MockReflog::Remove(const shash::Any &hash) {
  references_.erase(hash);
  return true;
}

bool MockReflog::ContainsCertificate(const shash::Any &certificate) const {
  return references_.count(certificate) == 1;
}

bool MockReflog::ContainsCatalog(const shash::Any &catalog) const {
  return references_.count(catalog) == 1;
}

bool MockReflog::ContainsHistory(const shash::Any &history) const {
  return references_.count(history) == 1;
}

bool MockReflog::ContainsMetainfo(const shash::Any &metainfo) const {
  return references_.count(metainfo) == 1;
}
