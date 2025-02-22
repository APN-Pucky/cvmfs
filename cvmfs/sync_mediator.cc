/**
 * This file is part of the CernVM File System.
 */

#define __STDC_FORMAT_MACROS

#include "sync_mediator.h"

#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "catalog_virtual.h"
#include "compression.h"
#include "crypto/hash.h"
#include "directory_entry.h"
#include "json_document.h"
#include "publish/repository.h"
#include "sync_union.h"
#include "upload.h"
#include "util/concurrency.h"
#include "util/exception.h"
#include "util/fs_traversal.h"
#include "util/posix.h"
#include "util/smalloc.h"
#include "util/string.h"

using namespace std;  // NOLINT

namespace publish {

AbstractSyncMediator::~AbstractSyncMediator() {}

SyncMediator::SyncMediator(catalog::WritableCatalogManager *catalog_manager,
                           const SyncParameters *params,
                           perf::StatisticsTemplate statistics)
    : catalog_manager_(catalog_manager)
    , union_engine_(NULL)
    , handle_hardlinks_(false)
    , params_(params)
    , reporter_(new SyncDiffReporter(params_->print_changeset
                                         ? SyncDiffReporter::kPrintChanges
                                         : SyncDiffReporter::kPrintDots)) {
  int retval = pthread_mutex_init(&lock_file_queue_, NULL);
  assert(retval == 0);

  params->spooler->RegisterListener(&SyncMediator::PublishFilesCallback, this);

  counters_ = new perf::FsCounters(statistics);
}

SyncMediator::~SyncMediator() {
  pthread_mutex_destroy(&lock_file_queue_);
}


void SyncMediator::RegisterUnionEngine(SyncUnion *engine) {
  union_engine_     = engine;
  handle_hardlinks_ = engine->SupportsHardlinks();
}


/**
 * The entry /.cvmfs or entries in /.cvmfs/ must not be added, removed or
 * modified manually.  The directory /.cvmfs is generated by the VirtualCatalog
 * class if requested.
 */
void SyncMediator::EnsureAllowed(SharedPtr<SyncItem> entry) {
  const bool ignore_case_setting = false;
  string relative_path = entry->GetRelativePath();
  if ( (relative_path == string(catalog::VirtualCatalog::kVirtualPath)) ||
       (HasPrefix(relative_path,
                  string(catalog::VirtualCatalog::kVirtualPath) + "/",
                  ignore_case_setting)) )
  {
    PANIC(kLogStderr, "[ERROR] invalid attempt to modify %s",
          relative_path.c_str());
  }
}


/**
 * Add an entry to the repository.
 * Added directories will be traversed in order to add the complete subtree.
 */
void SyncMediator::Add(SharedPtr<SyncItem> entry) {
  EnsureAllowed(entry);

  if (entry->IsDirectory()) {
    AddDirectoryRecursively(entry);
    return;
  }

  // .cvmfsbundles file type
  if (entry->IsBundleSpec()) {
    PrintWarning(".cvmfsbundles file encountered. "
                 "Bundles is currently an experimental feature.");

    if (!entry->IsRegularFile()) {
      PANIC(kLogStderr, "Error: .cvmfsbundles file must be a regular file");
    }
    if (entry->HasHardlinks()) {
      PANIC(kLogStderr, "Error: .cvmfsbundles file must not be a hard link");
    }

    std::string parent_path = GetParentPath(entry->GetUnionPath());
    if (parent_path != union_engine_->union_path()) {
      PANIC(kLogStderr, "Error: .cvmfsbundles file must be in the root"
            " directory of the repository. Found in %s", parent_path.c_str());
    }

    std::string json_string;

    int fd = open(entry->GetUnionPath().c_str(), O_RDONLY);
    if (fd < 0) {
      PANIC(kLogStderr, "Could not open file: %s",
            entry->GetUnionPath().c_str());
    }
    if (!SafeReadToString(fd, &json_string)) {
      PANIC(kLogStderr, "Could not read contents of file: %s",
            entry->GetUnionPath().c_str());
    }
    UniquePtr<JsonDocument> json(JsonDocument::Create(json_string));

    AddFile(entry);
    return;
  }

  if (entry->IsRegularFile() || entry->IsSymlink()) {
    // A file is a hard link if the link count is greater than 1
    if (entry->HasHardlinks() && handle_hardlinks_)
      InsertHardlink(entry);
    else
      AddFile(entry);
    return;
  } else if (entry->IsGraftMarker()) {
    LogCvmfs(kLogPublish, kLogDebug, "Ignoring graft marker file.");
    return;  // Ignore markers.
  }

  // In OverlayFS whiteouts can be represented as character devices with major
  // and minor numbers equal to 0. Special files will be ignored except if they
  // are whiteout files.
  if (entry->IsSpecialFile() && !entry->IsWhiteout()) {
    if (params_->ignore_special_files) {
      PrintWarning("'" + entry->GetRelativePath() + "' "
                  "is a special file, ignoring.");
    } else {
      if (entry->HasHardlinks() && handle_hardlinks_)
        InsertHardlink(entry);
      else
        AddFile(entry);
    }
    return;
  }

  PrintWarning("'" + entry->GetRelativePath() +
               "' cannot be added. Unrecognized file type.");
}


/**
 * Touch an entry in the repository.
 */
void SyncMediator::Touch(SharedPtr<SyncItem> entry) {
  EnsureAllowed(entry);

  if (entry->IsGraftMarker()) {return;}
  if (entry->IsDirectory()) {
    TouchDirectory(entry);
    perf::Inc(counters_->n_directories_changed);
    return;
  }

  if (entry->IsRegularFile() || entry->IsSymlink() || entry->IsSpecialFile()) {
    Replace(entry);  // This way, hardlink processing is correct
    // Replace calls Remove; cancel Remove's actions:
    perf::Xadd(counters_->sz_removed_bytes, -entry->GetRdOnlySize());

    // Count only the difference between the old and new file
    // Symlinks do not count into added or removed bytes
    int64_t dif = 0;

    // Need to handle 4 cases (symlink->symlink, symlink->regular,
    // regular->symlink, regular->regular)
    if (entry->WasSymlink()) {
      // Replace calls Remove; cancel Remove's actions:
      perf::Dec(counters_->n_symlinks_removed);

      if (entry->IsSymlink()) {
        perf::Inc(counters_->n_symlinks_changed);
      } else {
        perf::Inc(counters_->n_symlinks_removed);
        perf::Inc(counters_->n_files_added);
        dif += entry->GetScratchSize();
      }
    } else {
      // Replace calls Remove; cancel Remove's actions:
      perf::Dec(counters_->n_files_removed);
      dif -= entry->GetRdOnlySize();
      if (entry->IsSymlink()) {
        perf::Inc(counters_->n_files_removed);
        perf::Inc(counters_->n_symlinks_added);
      } else {
        perf::Inc(counters_->n_files_changed);
        dif += entry->GetScratchSize();
      }
    }

    if (dif > 0) {                            // added bytes
      perf::Xadd(counters_->sz_added_bytes, dif);
    } else {                                  // removed bytes
      perf::Xadd(counters_->sz_removed_bytes, -dif);
    }
    return;
  }

  PrintWarning("'" + entry->GetRelativePath() +
               "' cannot be touched. Unrecognized file type.");
}


/**
 * Remove an entry from the repository. Directories will be recursively removed.
 */
void SyncMediator::Remove(SharedPtr<SyncItem> entry) {
  EnsureAllowed(entry);

  if (entry->WasDirectory()) {
    RemoveDirectoryRecursively(entry);
    return;
  }

  if (entry->WasBundleSpec()) {
    // for now remove using RemoveFile()
    RemoveFile(entry);
    return;
  }

  if (entry->WasRegularFile() || entry->WasSymlink() ||
      entry->WasSpecialFile()) {
    RemoveFile(entry);
    return;
  }

  PrintWarning("'" + entry->GetRelativePath() +
               "' cannot be deleted. Unrecognized file type.");
}


/**
 * Remove the old entry and add the new one.
 */
void SyncMediator::Replace(SharedPtr<SyncItem> entry) {
  // EnsureAllowed(entry); <-- Done by Remove() and Add()
  Remove(entry);
  Add(entry);
}

void SyncMediator::Clone(const std::string from, const std::string to) {
  catalog_manager_->Clone(from, to);
}

void SyncMediator::EnterDirectory(SharedPtr<SyncItem> entry) {
  if (!handle_hardlinks_) {
    return;
  }

  HardlinkGroupMap new_map;
  hardlink_stack_.push(new_map);
}


void SyncMediator::LeaveDirectory(SharedPtr<SyncItem> entry)
{
  if (!handle_hardlinks_) {
    return;
  }

  CompleteHardlinks(entry);
  AddLocalHardlinkGroups(GetHardlinkMap());
  hardlink_stack_.pop();
}


/**
 * Do any pending processing and commit all changes to the catalogs.
 * To be called after change set traversal is finished.
 */
bool SyncMediator::Commit(manifest::Manifest *manifest) {
  reporter_->CommitReport();

  if (!params_->dry_run) {
    LogCvmfs(kLogPublish, kLogStdout,
             "Waiting for upload of files before committing...");
    params_->spooler->WaitForUpload();
  }

  if (!hardlink_queue_.empty()) {
    assert(handle_hardlinks_);

    LogCvmfs(kLogPublish, kLogStdout, "Processing hardlinks...");
    params_->spooler->UnregisterListeners();
    params_->spooler->RegisterListener(&SyncMediator::PublishHardlinksCallback,
                                       this);

    // TODO(rmeusel): Revise that for Thread Safety!
    //       This loop will spool hardlinks into the spooler, which will then
    //       process them.
    //       On completion of every hardlink the spooler will asynchronously
    //       emit callbacks (SyncMediator::PublishHardlinksCallback) which
    //       might happen while this for-loop goes through the hardlink_queue_
    //
    //       For the moment this seems not to be a problem, but it's an accident
    //       just waiting to happen.
    //
    //       Note: Just wrapping this loop in a mutex might produce a dead lock
    //             since the spooler does not fill it's processing queue to an
    //             unlimited size. Meaning that it might be flooded with hard-
    //             links and waiting for the queue to be processed while proces-
    //             sing is stalled because the callback is waiting for this
    //             mutex.
    for (HardlinkGroupList::const_iterator i = hardlink_queue_.begin(),
         iEnd = hardlink_queue_.end(); i != iEnd; ++i)
    {
      LogCvmfs(kLogPublish, kLogVerboseMsg, "Spooling hardlink group %s",
               i->master->GetUnionPath().c_str());
      IngestionSource *source =
          new FileIngestionSource(i->master->GetUnionPath());
      params_->spooler->Process(source);
    }

    params_->spooler->WaitForUpload();

    for (HardlinkGroupList::const_iterator i = hardlink_queue_.begin(),
         iEnd = hardlink_queue_.end(); i != iEnd; ++i)
    {
      LogCvmfs(kLogPublish, kLogVerboseMsg, "Processing hardlink group %s",
               i->master->GetUnionPath().c_str());
      AddHardlinkGroup(*i);
    }
  }

  if (union_engine_) union_engine_->PostUpload();

  params_->spooler->UnregisterListeners();

  if (params_->dry_run) {
    manifest = NULL;
    return true;
  }

  LogCvmfs(kLogPublish, kLogStdout, "Committing file catalogs...");
  if (params_->spooler->GetNumberOfErrors() > 0) {
    LogCvmfs(kLogPublish, kLogStderr, "failed to commit files");
    return false;
  }

  if (catalog_manager_->IsBalanceable() ||
      (params_->virtual_dir_actions != catalog::VirtualCatalog::kActionNone))
  {
    if (catalog_manager_->IsBalanceable())
      catalog_manager_->Balance();
    // Commit empty string to ensure that the "content" of the auto catalog
    // markers is present in the repository.
    string empty_file = CreateTempPath(params_->dir_temp + "/empty", 0600);
    IngestionSource* source = new FileIngestionSource(empty_file);
    params_->spooler->Process(source);
    params_->spooler->WaitForUpload();
    unlink(empty_file.c_str());
    if (params_->spooler->GetNumberOfErrors() > 0) {
      LogCvmfs(kLogPublish, kLogStderr, "failed to commit auto catalog marker");
      return false;
    }
  }
  catalog_manager_->PrecalculateListings();
  return catalog_manager_->Commit(params_->stop_for_catalog_tweaks,
                                  params_->manual_revision,
                                  manifest);
}


void SyncMediator::InsertHardlink(SharedPtr<SyncItem> entry) {
  assert(handle_hardlinks_);

  uint64_t inode = entry->GetUnionInode();
  LogCvmfs(kLogPublish, kLogVerboseMsg, "found hardlink %" PRIu64 " at %s",
           inode, entry->GetUnionPath().c_str());

  // Find the hard link group in the lists
  HardlinkGroupMap::iterator hardlink_group = GetHardlinkMap().find(inode);

  if (hardlink_group == GetHardlinkMap().end()) {
    // Create a new hardlink group
    GetHardlinkMap().insert(
      HardlinkGroupMap::value_type(inode, HardlinkGroup(entry)));
  } else {
    // Append the file to the appropriate hardlink group
    hardlink_group->second.AddHardlink(entry);
  }

  // publish statistics counting for new file
  if (entry->IsNew()) {
    perf::Inc(counters_->n_files_added);
    perf::Xadd(counters_->sz_added_bytes, entry->GetScratchSize());
  }
}


void SyncMediator::InsertLegacyHardlink(SharedPtr<SyncItem> entry) {
  // Check if found file has hardlinks (nlink > 1)
  // As we are looking through all files in one directory here, there might be
  // completely untouched hardlink groups, which we can safely skip.
  // Finally we have to see if the hardlink is already part of this group

  assert(handle_hardlinks_);

  if (entry->GetUnionLinkcount() < 2)
    return;

  uint64_t inode = entry->GetUnionInode();
  HardlinkGroupMap::iterator hl_group;
  hl_group = GetHardlinkMap().find(inode);

  if (hl_group != GetHardlinkMap().end()) {  // touched hardlinks in this group?
    bool found = false;

    // search for the entry in this group
    for (SyncItemList::const_iterator i = hl_group->second.hardlinks.begin(),
         iEnd = hl_group->second.hardlinks.end(); i != iEnd; ++i)
    {
      if (*(i->second) == *entry) {
        found = true;
        break;
      }
    }

    if (!found) {
      // Hardlink already in the group?
      // If one element of a hardlink group is edited, all elements must be
      // replaced.  Here, we remove an untouched hardlink and add it to its
      // hardlink group for re-adding later
      LogCvmfs(kLogPublish, kLogVerboseMsg, "Picked up legacy hardlink %s",
               entry->GetUnionPath().c_str());
      Remove(entry);
      hl_group->second.AddHardlink(entry);
    }
  }
}


/**
 * Create a recursion engine which DOES NOT recurse into directories.
 * It basically goes through the current directory (in the union volume) and
 * searches for legacy hardlinks which has to be connected to the new
 * or edited ones.
 */
void SyncMediator::CompleteHardlinks(SharedPtr<SyncItem> entry) {
  assert(handle_hardlinks_);

  // If no hardlink in this directory was changed, we can skip this
  if (GetHardlinkMap().empty())
    return;

  LogCvmfs(kLogPublish, kLogVerboseMsg, "Post-processing hard links in %s",
           entry->GetUnionPath().c_str());

  // Look for legacy hardlinks
  FileSystemTraversal<SyncMediator> traversal(this, union_engine_->union_path(),
                                              false);
  traversal.fn_new_file =
    &SyncMediator::LegacyRegularHardlinkCallback;
  traversal.fn_new_symlink = &SyncMediator::LegacySymlinkHardlinkCallback;
  traversal.fn_new_character_dev =
    &SyncMediator::LegacyCharacterDeviceHardlinkCallback;
  traversal.fn_new_block_dev = &SyncMediator::LegacyBlockDeviceHardlinkCallback;
  traversal.fn_new_fifo = &SyncMediator::LegacyFifoHardlinkCallback;
  traversal.fn_new_socket = &SyncMediator::LegacySocketHardlinkCallback;
  traversal.Recurse(entry->GetUnionPath());
}


void SyncMediator::LegacyRegularHardlinkCallback(const string &parent_dir,
                                                 const string &file_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, file_name, kItemFile);
  InsertLegacyHardlink(entry);
}


void SyncMediator::LegacySymlinkHardlinkCallback(const string &parent_dir,
                                                  const string &file_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemSymlink);
  InsertLegacyHardlink(entry);
}

void SyncMediator::LegacyCharacterDeviceHardlinkCallback(
  const string &parent_dir,
  const string &file_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemCharacterDevice);
  InsertLegacyHardlink(entry);
}

void SyncMediator::LegacyBlockDeviceHardlinkCallback(
  const string &parent_dir,
  const string &file_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemBlockDevice);
  InsertLegacyHardlink(entry);
}

void SyncMediator::LegacyFifoHardlinkCallback(const string &parent_dir,
                                              const string &file_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, file_name, kItemFifo);
  InsertLegacyHardlink(entry);
}

void SyncMediator::LegacySocketHardlinkCallback(const string &parent_dir,
                                                const string &file_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemSocket);
  InsertLegacyHardlink(entry);
}


void SyncMediator::AddDirectoryRecursively(SharedPtr<SyncItem> entry) {
  AddDirectory(entry);

  // Create a recursion engine, which recursively adds all entries in a newly
  // created directory
  FileSystemTraversal<SyncMediator> traversal(
    this, union_engine_->scratch_path(), true);
  traversal.fn_enter_dir      = &SyncMediator::EnterAddedDirectoryCallback;
  traversal.fn_leave_dir      = &SyncMediator::LeaveAddedDirectoryCallback;
  traversal.fn_new_file       = &SyncMediator::AddFileCallback;
  traversal.fn_new_symlink    = &SyncMediator::AddSymlinkCallback;
  traversal.fn_new_dir_prefix = &SyncMediator::AddDirectoryCallback;
  traversal.fn_ignore_file    = &SyncMediator::IgnoreFileCallback;
  traversal.fn_new_character_dev = &SyncMediator::AddCharacterDeviceCallback;
  traversal.fn_new_block_dev = &SyncMediator::AddBlockDeviceCallback;
  traversal.fn_new_fifo      = &SyncMediator::AddFifoCallback;
  traversal.fn_new_socket    = &SyncMediator::AddSocketCallback;
  traversal.Recurse(entry->GetScratchPath());
}


bool SyncMediator::AddDirectoryCallback(const std::string &parent_dir,
                                        const std::string &dir_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, dir_name, kItemDir);
  AddDirectory(entry);
  return true;  // The recursion engine should recurse deeper here
}


void SyncMediator::AddFileCallback(const std::string &parent_dir,
                                   const std::string &file_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, file_name, kItemFile);
  Add(entry);
}


void SyncMediator::AddCharacterDeviceCallback(const std::string &parent_dir,
                                    const std::string &file_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemCharacterDevice);
  Add(entry);
}

void SyncMediator::AddBlockDeviceCallback(const std::string &parent_dir,
                                    const std::string &file_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemBlockDevice);
  Add(entry);
}

void SyncMediator::AddFifoCallback(const std::string &parent_dir,
                                   const std::string &file_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, file_name, kItemFifo);
  Add(entry);
}

void SyncMediator::AddSocketCallback(const std::string &parent_dir,
                                     const std::string &file_name) {
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemSocket);
  Add(entry);
}

void SyncMediator::AddSymlinkCallback(const std::string &parent_dir,
                                      const std::string &link_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, link_name, kItemSymlink);
  Add(entry);
}


void SyncMediator::EnterAddedDirectoryCallback(const std::string &parent_dir,
                                               const std::string &dir_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, dir_name, kItemDir);
  EnterDirectory(entry);
}


void SyncMediator::LeaveAddedDirectoryCallback(const std::string &parent_dir,
                                               const std::string &dir_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, dir_name, kItemDir);
  LeaveDirectory(entry);
}


void SyncMediator::RemoveDirectoryRecursively(SharedPtr<SyncItem> entry) {
  // Delete a directory AFTER it was emptied here,
  // because it would start up another recursion

  const bool recurse = false;
  FileSystemTraversal<SyncMediator> traversal(
    this, union_engine_->rdonly_path(), recurse);
  traversal.fn_new_file = &SyncMediator::RemoveFileCallback;
  traversal.fn_new_dir_postfix =
    &SyncMediator::RemoveDirectoryCallback;
  traversal.fn_new_symlink = &SyncMediator::RemoveSymlinkCallback;
  traversal.fn_new_character_dev = &SyncMediator::RemoveCharacterDeviceCallback;
  traversal.fn_new_block_dev = &SyncMediator::RemoveBlockDeviceCallback;
  traversal.fn_new_fifo      = &SyncMediator::RemoveFifoCallback;
  traversal.fn_new_socket    = &SyncMediator::RemoveSocketCallback;
  traversal.Recurse(entry->GetRdOnlyPath());

  // The given directory was emptied recursively and can now itself be deleted
  RemoveDirectory(entry);
}


void SyncMediator::RemoveFileCallback(const std::string &parent_dir,
                                      const std::string &file_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, file_name, kItemFile);
  Remove(entry);
}


void SyncMediator::RemoveSymlinkCallback(const std::string &parent_dir,
                                         const std::string &link_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, link_name, kItemSymlink);
  Remove(entry);
}

void SyncMediator::RemoveCharacterDeviceCallback(
  const std::string &parent_dir,
  const std::string &link_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, link_name, kItemCharacterDevice);
  Remove(entry);
}

void SyncMediator::RemoveBlockDeviceCallback(
  const std::string &parent_dir,
  const std::string &link_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, link_name, kItemBlockDevice);
  Remove(entry);
}

void SyncMediator::RemoveFifoCallback(const std::string &parent_dir,
                                      const std::string &link_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, link_name, kItemFifo);
  Remove(entry);
}

void SyncMediator::RemoveSocketCallback(const std::string &parent_dir,
                                        const std::string &link_name)
{
  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, link_name, kItemSocket);
  Remove(entry);
}

void SyncMediator::RemoveDirectoryCallback(const std::string &parent_dir,
                                           const std::string &dir_name)
{
  SharedPtr<SyncItem> entry = CreateSyncItem(parent_dir, dir_name, kItemDir);
  RemoveDirectoryRecursively(entry);
}


bool SyncMediator::IgnoreFileCallback(const std::string &parent_dir,
                                      const std::string &file_name)
{
  if (union_engine_->IgnoreFilePredicate(parent_dir, file_name)) {
    return true;
  }

  SharedPtr<SyncItem> entry =
      CreateSyncItem(parent_dir, file_name, kItemUnknown);
  return entry->IsWhiteout();
}

SharedPtr<SyncItem> SyncMediator::CreateSyncItem(
    const std::string &relative_parent_path, const std::string &filename,
    const SyncItemType entry_type) const {
  return union_engine_->CreateSyncItem(relative_parent_path, filename,
                                       entry_type);
}

void SyncMediator::PublishFilesCallback(const upload::SpoolerResult &result) {
  LogCvmfs(kLogPublish, kLogVerboseMsg,
           "Spooler callback for %s, digest %s, produced %d chunks, retval %d",
           result.local_path.c_str(),
           result.content_hash.ToString().c_str(),
           result.file_chunks.size(),
           result.return_code);
  if (result.return_code != 0) {
    PANIC(kLogStderr, "Spool failure for %s (%d)", result.local_path.c_str(),
          result.return_code);
  }

  SyncItemList::iterator itr;
  {
    MutexLockGuard guard(lock_file_queue_);
    itr = file_queue_.find(result.local_path);
  }

  assert(itr != file_queue_.end());

  SyncItem &item = *itr->second;
  item.SetContentHash(result.content_hash);
  item.SetCompressionAlgorithm(result.compression_alg);

  XattrList *xattrs = &default_xattrs_;
  if (params_->include_xattrs) {
    xattrs = XattrList::CreateFromFile(result.local_path);
    assert(xattrs != NULL);
  }

  if (result.IsChunked()) {
    catalog_manager_->AddChunkedFile(
      item.CreateBasicCatalogDirent(),
      *xattrs,
      item.relative_parent_path(),
      result.file_chunks);
  } else {
    catalog_manager_->AddFile(
      item.CreateBasicCatalogDirent(),
      *xattrs,
      item.relative_parent_path());
  }

  if (xattrs != &default_xattrs_)
    free(xattrs);
}


void SyncMediator::PublishHardlinksCallback(
  const upload::SpoolerResult &result)
{
  LogCvmfs(kLogPublish, kLogVerboseMsg,
           "Spooler callback for hardlink %s, digest %s, retval %d",
           result.local_path.c_str(),
           result.content_hash.ToString().c_str(),
           result.return_code);
  if (result.return_code != 0) {
    PANIC(kLogStderr, "Spool failure for %s (%d)", result.local_path.c_str(),
          result.return_code);
  }

  bool found = false;
  for (unsigned i = 0; i < hardlink_queue_.size(); ++i) {
    if (hardlink_queue_[i].master->GetUnionPath() == result.local_path) {
      found = true;
      hardlink_queue_[i].master->SetContentHash(result.content_hash);
      SyncItemList::iterator j, jend;
      for (j = hardlink_queue_[i].hardlinks.begin(),
           jend = hardlink_queue_[i].hardlinks.end();
           j != jend; ++j)
      {
        j->second->SetContentHash(result.content_hash);
        j->second->SetCompressionAlgorithm(result.compression_alg);
      }
      if (result.IsChunked())
        hardlink_queue_[i].file_chunks = result.file_chunks;

      break;
    }
  }

  assert(found);
}


void SyncMediator::CreateNestedCatalog(SharedPtr<SyncItem> directory) {
  const std::string notice = "Nested catalog at " + directory->GetUnionPath();
  reporter_->OnAdd(notice, catalog::DirectoryEntry());

  if (!params_->dry_run) {
    catalog_manager_->CreateNestedCatalog(directory->GetRelativePath());
  }
}


void SyncMediator::RemoveNestedCatalog(SharedPtr<SyncItem> directory) {
  const std::string notice =  "Nested catalog at " + directory->GetUnionPath();
  reporter_->OnRemove(notice, catalog::DirectoryEntry());

  if (!params_->dry_run) {
    catalog_manager_->RemoveNestedCatalog(directory->GetRelativePath());
  }
}

void SyncDiffReporter::OnInit(const history::History::Tag & /*from_tag*/,
                              const history::History::Tag & /*to_tag*/) {}

void SyncDiffReporter::OnStats(const catalog::DeltaCounters & /*delta*/) {}

void SyncDiffReporter::OnAdd(const std::string &path,
                             const catalog::DirectoryEntry & /*entry*/) {
  changed_items_++;
  AddImpl(path);
}
void SyncDiffReporter::OnRemove(const std::string &path,
                                const catalog::DirectoryEntry & /*entry*/) {
  changed_items_++;
  RemoveImpl(path);
}
void SyncDiffReporter::OnModify(const std::string &path,
                                const catalog::DirectoryEntry & /*entry_from*/,
                                const catalog::DirectoryEntry & /*entry_to*/) {
  changed_items_++;
  ModifyImpl(path);
}

void SyncDiffReporter::CommitReport() {
  if (print_action_ == kPrintDots) {
    if (changed_items_ >= processing_dot_interval_) {
      LogCvmfs(kLogPublish, kLogStdout, "");
    }
  }
}

void SyncDiffReporter::PrintDots() {
  if (changed_items_ % processing_dot_interval_ == 0) {
    LogCvmfs(kLogPublish, kLogStdout | kLogNoLinebreak, ".");
  }
}

void SyncDiffReporter::AddImpl(const std::string &path) {
  const char *action_label;

  switch (print_action_) {
    case kPrintChanges:
      if (path.at(0) != '/') {
        action_label = "[x-catalog-add]";
      } else {
        action_label = "[add]";
      }
      LogCvmfs(kLogPublish, kLogStdout, "%s %s", action_label, path.c_str());
      break;

    case kPrintDots:
      PrintDots();
      break;
    default:
      assert("Invalid print action.");
  }
}

void SyncDiffReporter::RemoveImpl(const std::string &path) {
  const char *action_label;

  switch (print_action_) {
    case kPrintChanges:
      if (path.at(0) != '/') {
        action_label = "[x-catalog-rem]";
      } else {
        action_label = "[rem]";
      }

      LogCvmfs(kLogPublish, kLogStdout, "%s %s", action_label, path.c_str());
      break;

    case kPrintDots:
      PrintDots();
      break;
    default:
      assert("Invalid print action.");
  }
}

void SyncDiffReporter::ModifyImpl(const std::string &path) {
  const char *action_label;

  switch (print_action_) {
    case kPrintChanges:
      action_label = "[mod]";
      LogCvmfs(kLogPublish, kLogStdout, "%s %s", action_label, path.c_str());
      break;

    case kPrintDots:
      PrintDots();
      break;
    default:
      assert("Invalid print action.");
  }
}

void SyncMediator::AddFile(SharedPtr<SyncItem> entry) {
  reporter_->OnAdd(entry->GetUnionPath(), catalog::DirectoryEntry());

  if ((entry->IsSymlink() || entry->IsSpecialFile()) && !params_->dry_run) {
    assert(!entry->HasGraftMarker());
    // Symlinks and special files are completely stored in the catalog
    XattrList *xattrs = &default_xattrs_;
    if (params_->include_xattrs) {
      xattrs = XattrList::CreateFromFile(entry->GetUnionPath());
      assert(xattrs);
    }
    catalog_manager_->AddFile(entry->CreateBasicCatalogDirent(), *xattrs,
                              entry->relative_parent_path());
    if (xattrs != &default_xattrs_)
      free(xattrs);
  } else if (entry->HasGraftMarker() && !params_->dry_run) {
    if (entry->IsValidGraft()) {
      // Graft files are added to catalog immediately.
      if (entry->IsChunkedGraft()) {
        catalog_manager_->AddChunkedFile(
            entry->CreateBasicCatalogDirent(), default_xattrs_,
            entry->relative_parent_path(), *(entry->GetGraftChunks()));
      } else {
        catalog_manager_->AddFile(
            entry->CreateBasicCatalogDirent(),
            default_xattrs_,  // TODO(bbockelm): For now, use default xattrs
                              // on grafted files.
            entry->relative_parent_path());
      }
    } else {
      // Unlike with regular files, grafted files can be "unpublishable" - i.e.,
      // the graft file is missing information.  It's not clear that continuing
      // forward with the publish is the correct thing to do; abort for now.
      PANIC(kLogStderr,
            "Encountered a grafted file (%s) with "
            "invalid grafting information; check contents of .cvmfsgraft-*"
            " file.  Aborting publish.",
            entry->GetRelativePath().c_str());
    }
  } else if (entry->relative_parent_path().empty() &&
             entry->IsCatalogMarker()) {
    PANIC(kLogStderr, "Error: nested catalog marker in root directory");
  } else if (!params_->dry_run) {
    {
      // Push the file to the spooler, remember the entry for the path
      MutexLockGuard m(&lock_file_queue_);
      file_queue_[entry->GetUnionPath()] = entry;
    }
    // Spool the file
    params_->spooler->Process(entry->CreateIngestionSource());
  }

  // publish statistics counting for new file
  if (entry->IsNew()) {
    if (entry->IsSymlink()) {
      perf::Inc(counters_->n_symlinks_added);
    } else {
      perf::Inc(counters_->n_files_added);
      perf::Xadd(counters_->sz_added_bytes, entry->GetScratchSize());
    }
  }
}

void SyncMediator::RemoveFile(SharedPtr<SyncItem> entry) {
  reporter_->OnRemove(entry->GetUnionPath(), catalog::DirectoryEntry());

  if (!params_->dry_run) {
    if (handle_hardlinks_ && entry->GetRdOnlyLinkcount() > 1) {
      LogCvmfs(kLogPublish, kLogVerboseMsg, "remove %s from hardlink group",
               entry->GetUnionPath().c_str());
      catalog_manager_->ShrinkHardlinkGroup(entry->GetRelativePath());
    }
    catalog_manager_->RemoveFile(entry->GetRelativePath());
  }

  // Counting nr of removed files and removed bytes
  if (entry->WasSymlink()) {
    perf::Inc(counters_->n_symlinks_removed);
  } else {
    perf::Inc(counters_->n_files_removed);
  }
  perf::Xadd(counters_->sz_removed_bytes, entry->GetRdOnlySize());
}

void SyncMediator::AddUnmaterializedDirectory(SharedPtr<SyncItem> entry) {
  AddDirectory(entry);
}

void SyncMediator::AddDirectory(SharedPtr<SyncItem> entry) {
  if (entry->IsBundleSpec()) {
    PANIC(kLogStderr, "Illegal directory name: .cvmfsbundles (%s). "
          ".cvmfsbundles is reserved for bundles specification files",
          entry->GetUnionPath().c_str());
  }

  reporter_->OnAdd(entry->GetUnionPath(), catalog::DirectoryEntry());

  perf::Inc(counters_->n_directories_added);
  assert(!entry->HasGraftMarker());
  if (!params_->dry_run) {
    XattrList *xattrs = &default_xattrs_;
    if (params_->include_xattrs) {
      xattrs = XattrList::CreateFromFile(entry->GetUnionPath());
      assert(xattrs);
    }
    catalog_manager_->AddDirectory(entry->CreateBasicCatalogDirent(), *xattrs,
                                   entry->relative_parent_path());
    if (xattrs != &default_xattrs_)
      free(xattrs);
  }

  if (entry->HasCatalogMarker() &&
      !catalog_manager_->IsTransitionPoint(entry->GetRelativePath())) {
    CreateNestedCatalog(entry);
  }
}


/**
 * this method deletes a single directory entry! Make sure to empty it
 * before you call this method or simply use
 * SyncMediator::RemoveDirectoryRecursively instead.
 */
void SyncMediator::RemoveDirectory(SharedPtr<SyncItem> entry) {
  const std::string directory_path = entry->GetRelativePath();

  if (catalog_manager_->IsTransitionPoint(directory_path)) {
    RemoveNestedCatalog(entry);
  }

  reporter_->OnRemove(entry->GetUnionPath(), catalog::DirectoryEntry());
  if (!params_->dry_run) {
    catalog_manager_->RemoveDirectory(directory_path);
  }

  perf::Inc(counters_->n_directories_removed);
}

void SyncMediator::TouchDirectory(SharedPtr<SyncItem> entry) {
  reporter_->OnModify(entry->GetUnionPath(), catalog::DirectoryEntry(),
                      catalog::DirectoryEntry());

  const std::string directory_path = entry->GetRelativePath();

  if (!params_->dry_run) {
    XattrList *xattrs = &default_xattrs_;
    if (params_->include_xattrs) {
      xattrs = XattrList::CreateFromFile(entry->GetUnionPath());
      assert(xattrs);
    }
    catalog_manager_->TouchDirectory(entry->CreateBasicCatalogDirent(), *xattrs,
                                     directory_path);
    if (xattrs != &default_xattrs_) free(xattrs);
  }

  if (entry->HasCatalogMarker() &&
      !catalog_manager_->IsTransitionPoint(directory_path)) {
    CreateNestedCatalog(entry);
  } else if (!entry->HasCatalogMarker() &&
             catalog_manager_->IsTransitionPoint(directory_path)) {
    RemoveNestedCatalog(entry);
  }
}

/**
 * All hardlinks in the current directory have been picked up.  Now they are
 * added to the catalogs.
 */
void SyncMediator::AddLocalHardlinkGroups(const HardlinkGroupMap &hardlinks) {
  assert(handle_hardlinks_);

  for (HardlinkGroupMap::const_iterator i = hardlinks.begin(),
       iEnd = hardlinks.end(); i != iEnd; ++i)
  {
    if (i->second.hardlinks.size() != i->second.master->GetUnionLinkcount() &&
        !params_->ignore_xdir_hardlinks) {
      PANIC(kLogSyslogErr | kLogDebug, "Hardlinks across directories (%s)",
            i->second.master->GetUnionPath().c_str());
    }

    if (params_->print_changeset) {
      for (SyncItemList::const_iterator j = i->second.hardlinks.begin(),
           jEnd = i->second.hardlinks.end(); j != jEnd; ++j)
      {
        std::string changeset_notice =
            GetParentPath(i->second.master->GetUnionPath()) + "/" +
            j->second->filename();
        reporter_->OnAdd(changeset_notice, catalog::DirectoryEntry());
      }
    }

    if (params_->dry_run)
      continue;

    if (i->second.master->IsSymlink() || i->second.master->IsSpecialFile())
      AddHardlinkGroup(i->second);
    else
      hardlink_queue_.push_back(i->second);
  }
}


void SyncMediator::AddHardlinkGroup(const HardlinkGroup &group) {
  assert(handle_hardlinks_);

  // Create a DirectoryEntry list out of the hardlinks
  catalog::DirectoryEntryBaseList hardlinks;
  for (SyncItemList::const_iterator i = group.hardlinks.begin(),
       iEnd = group.hardlinks.end(); i != iEnd; ++i)
  {
    hardlinks.push_back(i->second->CreateBasicCatalogDirent());
  }
  XattrList *xattrs = &default_xattrs_;
  if (params_->include_xattrs) {
    xattrs = XattrList::CreateFromFile(group.master->GetUnionPath());
    assert(xattrs);
  }
  catalog_manager_->AddHardlinkGroup(
    hardlinks,
    *xattrs,
    group.master->relative_parent_path(),
    group.file_chunks);
  if (xattrs != &default_xattrs_)
    free(xattrs);
}

}  // namespace publish
