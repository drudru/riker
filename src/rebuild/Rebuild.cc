#include "Rebuild.hh"

#include <cerrno>
#include <ctime>
#include <iostream>
#include <memory>
#include <ostream>
#include <queue>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "data/Command.hh"
#include "data/IR.hh"
#include "data/Version.hh"
#include "tracing/Tracer.hh"
#include "ui/log.hh"
#include "ui/options.hh"

using std::cout;
using std::dynamic_pointer_cast;
using std::endl;
using std::make_shared;
using std::ostream;
using std::queue;

// Create a rebuild plan
Rebuild Rebuild::create(shared_ptr<Command> root) {
  // Initialize the rebuild with the build's root command
  Rebuild r(root);

  // Identify commands with changed dependencies
  r.findChanges(root);

  // Check the final outputs of the emulated build against the filesystem
  r.checkFinalState();

  // Mark all the commands with changed inputs
  for (auto& c : r._changed) {
    c->mark();
  }

  // Mark all the commands whose output is required
  for (auto& c : r._output_needed) {
    c->mark();
  }

  return r;
}

// Run a rebuild, updating the in-memory build representation
void Rebuild::run() {
  // Create a tracing context to run the build
  Tracer tracer(*this);

  // Run or emulate the root command with the tracer
  runCommand(_root, tracer);

  // Finish up by saving metadata for any remaining artifacts
  for (auto& [_, entry] : _artifacts) {
    auto& [path, artifact] = entry;
    auto ref = make_shared<Access>(path, AccessFlags());
    artifact->saveMetadata();
    artifact->saveFingerprint();
  }
}

// Run or emulate a command in this rebuild
void Rebuild::runCommand(shared_ptr<Command> c, Tracer& tracer) {
  // Does the rebuild plan say command c must run?
  if (c->mustRerun()) {
    // We are rerunning this command, so clear the lists of steps and children
    c->reset();

    // Show the command if printing is on, or if this is a dry run
    if (options::print_on_run || options::dry_run) cout << c->getFullName() << endl;

    // Actually run the command, unless this is a dry run
    if (!options::dry_run) tracer.run(c);

  } else {
    // Emulate this command by running its children
    for (auto& child : c->getChildren()) {
      runCommand(child, tracer);
    }
  }
}

// Get an artifact during tracing
Artifact Rebuild::getArtifact(shared_ptr<Command> c, shared_ptr<Reference> ref, bool created) {
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    // Look to see if we've already resolve this reference to an artifact
    auto iter = _pipes.find(p);
    if (iter == _pipes.end()) {
      // This is a new pipe. Create an initial version for the pipe
      auto v = make_shared<OpenedVersion>(p);

      // Add the record for this pipe
      iter = _pipes.emplace_hint(iter, p, Artifact(v));
    }

    // Return the artifact, which was either found or inserted
    return iter->second;

  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    string p = a->getPath();
    bool follow_links = !a->getFlags().nofollow;

    // Now that we have a path, we can stat it
    struct stat statbuf;
    int rc;
    if (follow_links) {
      rc = stat(p.c_str(), &statbuf);
    } else {
      rc = lstat(p.c_str(), &statbuf);
    }

    // If the stat call failed, return an empty artifact
    if (rc) return Artifact();

    // The stat call succeeded. Check for an existing inode entry
    auto iter = _artifacts.find(statbuf.st_ino);
    if (iter == _artifacts.end()) {
      shared_ptr<Version> v;

      // Did the reference create this artifact?
      if (created) {
        // Yes. The version is
        auto s = make_shared<SetContents>(c, ref);

        // The command also sets the version's contents
        c->addStep(s);

        // The SetContents action is also the current version
        v = s;

      } else {
        // No. This version is just opened
        v = make_shared<OpenedVersion>(ref);
      }

      // Add the artifact to the map
      iter = _artifacts.emplace_hint(iter, statbuf.st_ino, pair<string, Artifact>{p, Artifact(v)});
    }

    // Return the found/inserted artifact
    return iter->second.second;
  } else {
    FAIL << "Unsupported reference type";
    abort();
  }
}

/// The command accesses an artifact by path.
shared_ptr<Reference> Rebuild::access(shared_ptr<Command> c, string path, AccessFlags flags) {
  auto ref = make_shared<Access>(path, flags);
  c->addStep(ref);
  return ref;
}

/// This command creates a reference to a new pipe
shared_ptr<Reference> Rebuild::pipe(shared_ptr<Command> c) {
  auto ref = make_shared<Pipe>();
  c->addStep(ref);
  return ref;
}

/// This command observes a reference resolve with a particular result
void Rebuild::referenceResult(shared_ptr<Command> c, shared_ptr<Reference> ref, int result) {
  c->addStep(make_shared<ReferenceResult>(ref, result));
}

/// This command accesses the metadata for an artifact
void Rebuild::metadataMatch(shared_ptr<Command> c, shared_ptr<Reference> ref, const Artifact& a) {
  // When the optimization is enabled, we can assume that a command sees its own writes without
  // having to record the dependency. This is always safe.
  if (options::ignore_self_reads && a->getCreator() == c) return;

  // Add this check to the set of metadata checks. If the check is not new, we can return.
  if (options::skip_repeat_checks && !c->checkMetadataRequired(ref, a)) return;

  // The version has been accessed
  a->setAccessed();

  // Make sure we have metadata saved for that version
  a->saveMetadata();

  // Record the dependency on metadata
  c->addStep(make_shared<MetadataMatch>(ref, a));
}

/// This command accesses the contents of an artifact
void Rebuild::contentsMatch(shared_ptr<Command> c, shared_ptr<Reference> ref, const Artifact& a) {
  // When the optimization is enabled, we can assume that a command sees its own writes without
  // having to record the dependency. This is always safe.
  if (options::ignore_self_reads && a->getCreator() == c) return;

  // Add this check to the set of contents checks. If the check is not new, we can return.
  if (options::skip_repeat_checks && !c->checkContentsRequired(ref, a)) return;

  // The version has been accessed
  a->setAccessed();

  // Make sure we have a fingerprint saved for this version
  a->saveFingerprint();

  // Record the dependency
  c->addStep(make_shared<ContentsMatch>(ref, a));
}

/// This command sets the metadata for an artifact
void Rebuild::setMetadata(shared_ptr<Command> c, shared_ptr<Reference> ref, const Artifact& a) {
  // We cannot do write-combining on metadata updates because any access to a path could depend on
  // an update to the metadata of any artifact along that path (e.g. /, /foo, /foo/bar, ...)

  // Create the SetMetadata step and add it to the command
  auto s = make_shared<SetMetadata>(c, ref);
  c->addStep(s);

  // The SetMetadata step is also the new version of Artifact a
  a->followedBy(s);
}

/// This command sets the contents of an artifact
void Rebuild::setContents(shared_ptr<Command> c, shared_ptr<Reference> ref, const Artifact& a) {
  // If this command created the last version, and no other command has accessed it, we can
  // combine the updates into a single update. That means we don't need to tag a new version.
  if (options::combine_writes && a->getCreator() == c && !a->isAccessed()) return;

  // If we reach this point, the command is creating a new version of the artifact
  // Create the SetContents step and add it to the command
  auto s = make_shared<SetContents>(c, ref);
  c->addStep(s);

  // The SetContents step is also the new version of Artifact a
  a->followedBy(s);
}

/// This command launches a child command
shared_ptr<Command> Rebuild::launch(shared_ptr<Command> c, string exe, vector<string> args,
                                    map<int, InitialFD> fds) {
  auto child = make_shared<Command>(exe, args, fds);

  if (options::print_on_run) cout << child->getFullName() << endl;

  c->addStep(make_shared<Launch>(child));
  c->addChild(child);
  return child;
}

// Show rebuild information
ostream& Rebuild::print(ostream& o) const {
  if (_changed.size() > 0) {
    o << "Commands with changed inputs:" << endl;
    for (auto& c : _changed) {
      o << "  " << c << endl;
    }
    o << endl;
  }

  if (_output_needed.size() > 0) {
    o << "Commands whose output is missing or modified:" << endl;
    for (auto& c : _output_needed) {
      o << "  " << c << endl;
    }
    o << endl;
  }

  if (_changed.size() > 0 || _output_needed.size() > 0) {
    o << "A rebuild will run the following commands:" << endl;
    queue<shared_ptr<Command>> q;
    q.push(_root);
    while (!q.empty()) {
      auto c = q.front();
      o << "  " << c << endl;
      q.pop();
      for (auto& child : c->getChildren()) {
        q.push(child);
      }
    }
  } else {
    o << "No changes detected" << endl;
  }

  return o;
}

void Rebuild::findChanges(shared_ptr<Command> c) {
  // Keep track of whether we've seen any changes for command c
  bool changed = false;

  // If this command has never run, it is definitely changed
  if (c->neverRun()) {
    LOG << c << " changed: never run";
    changed = true;
  }

  // Loop over the steps from the command trace to see if command c will see any changes
  for (auto step : c->getSteps()) {
    // Handle each IR type here
    if (auto pipe = dynamic_pointer_cast<Pipe>(step)) {
      // Nothing to do for pipes

    } else if (auto access = dynamic_pointer_cast<Access>(step)) {
      // Nothing to do for access

    } else if (auto ref_result = dynamic_pointer_cast<ReferenceResult>(step)) {
      // Check if the reference resolves the same way
      if (!checkAccess(c, ref_result->getReference(), ref_result->getResult())) {
        LOG << c << " changed: " << ref_result;
        changed = true;
      }

    } else if (auto m_match = dynamic_pointer_cast<MetadataMatch>(step)) {
      // Check if the metadata in the environment matches the expected version
      if (!checkMetadata(c, m_match->getReference(), m_match->getVersion())) {
        LOG << c << " changed: " << m_match;
        changed = true;
      }

    } else if (auto c_match = dynamic_pointer_cast<ContentsMatch>(step)) {
      // Check if the contents in the environment match the expected version
      if (!checkContents(c, c_match->getReference(), c_match->getVersion())) {
        LOG << c << " changed: " << c_match;
        changed = true;
      }

    } else if (auto launch = dynamic_pointer_cast<Launch>(step)) {
      // Check the child command's inputs
      findChanges(launch->getCommand());

    } else if (auto set_metadata = dynamic_pointer_cast<SetMetadata>(step)) {
      // Update the metadata in the environment
      setMetadata(c, set_metadata->getReference(), set_metadata);

    } else if (auto set_contents = dynamic_pointer_cast<SetContents>(step)) {
      // Update the contents in the environment
      setContents(c, set_contents->getReference(), set_contents);

    } else {
      FAIL << "Unsupported IR action " << step;
    }
  }

  // If anything was different, add c to the set of commands with changed inputs
  if (changed) _changed.insert(c);
}

void Rebuild::checkFinalState() {
  // Loop over all entries in the environment
  for (auto& [path, entry] : _entries) {
    // Create a fake reference to check the file, just for now
    auto ref = make_shared<Access>(path, AccessFlags());

    // Check the filesystem to see if the real file matches our expected version
    if (!checkFilesystemContents(ref, entry)) {
      _output_needed.insert(entry->getCreator());
    }
  }
}

// Check if an access resolves as-expected in the current environment
bool Rebuild::checkAccess(shared_ptr<Command> c, shared_ptr<Reference> ref, int expected) {
  // Is ref a pipe, access, or something else?
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    WARN << "Warning: Communication through pipes is not yet tracked correctly.";
    // TODO: keep track of pipes in the environment, maybe?
    // Creating a pipe reference always succeeds. Check if SUCCESS was expected
    return expected == SUCCESS;

  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    // Look for the reference's path in the current environment
    // TODO: handle the nofollow flag
    // TODO: handle permissions
    // TODO: handle changes to directories along the path used by ref
    auto iter = _entries.find(a->getPath());
    if (iter != _entries.end()) {
      // Get the writing command and the entry it wrote
      auto [path, entry] = *iter;

      // In a rebuild that runs entry->getCreator(), c must also run because it consumes output
      // from entry->getCreator().
      entry->getCreator()->outputUsedBy(c);

      // If we have a cached copy of the version c accesses, there's no need to rerun that
      // version's creator just to produce the file.
      if (options::enable_cache && entry->isSaved()) {
        // We can use the cached version of the file
      } else {
        // No cached version available, so rerun the version's creator any time c reruns
        c->needsOutputFrom(entry->getCreator());
      }

      // This access will succeed, so check if that matches the expected outcome
      return expected == SUCCESS;
    } else {
      // There was no entry in the environment. Check the actual filesystem
      return checkFilesystemAccess(a, expected);
    }

  } else {
    WARN << "Unsupported reference type: " << ref;
    return false;
  }
}

bool Rebuild::checkMetadata(shared_ptr<Command> c, shared_ptr<Reference> ref,
                            shared_ptr<Version> v) {
  // Is ref a pipe, access, or something else?
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    // TODO: Handle pipes correctly.
    // For now, we'll just say pipe metadata is always different (i.e. it does not match v)
    return false;

  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    // TODO: handle nofollow
    // Look for this reference in the current environment
    auto iter = _entries.find(a->getPath());
    if (iter != _entries.end()) {
      // Found a matching entry
      auto [path, entry] = *iter;

      // If the writer ever reruns, the current command must rerun as well. Record that.
      entry->getCreator()->outputUsedBy(c);

      // If we have a cached copy of the version c accesses, there's no need to rerun that
      // version's creator just to produce the file.
      if (options::enable_cache && entry->hasMetadata()) {
        // We can use the cached version
      } else {
        // No cached version available, so rerun the version's creator any time c reruns
        c->needsOutputFrom(entry->getCreator());
      }

      // Does the current version in the environment match the expected version?
      return entry->metadataMatch(v);
    } else {
      // There is no matching entry in the environment. Check the actual filesystem
      return checkFilesystemMetadata(a, v);
    }

  } else {
    WARN << "Unsupported reference type: " << ref;
    return false;
  }
}

bool Rebuild::checkContents(shared_ptr<Command> c, shared_ptr<Reference> ref,
                            shared_ptr<Version> v) {
  // Is ref a pipe, access, or something else?
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    // TODO: Handle pipes correctly.
    // For now, we'll just say pipe metadata is always different (i.e. it does not match v)
    return false;

  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    // TODO: handle nofollow
    // Look for this reference in the current environment
    auto iter = _entries.find(a->getPath());
    if (iter != _entries.end()) {
      // Found a matching entry
      auto [path, entry] = *iter;

      // If the writer ever reruns, the current command must rerun as well. Record that.
      entry->getCreator()->outputUsedBy(c);

      // If we have a cached copy of the version c accesses, there's no need to rerun that
      // version's creator just to produce the file.
      if (options::enable_cache && entry->isSaved()) {
        // We can use the cached version of the file
      } else {
        // No cached version available, so rerun the version's creator any time c reruns
        c->needsOutputFrom(entry->getCreator());
      }

      // Does the current version in the environment match the expected version?
      return entry->fingerprintMatch(v);
    } else {
      // There is no matching entry in the environment. Check the actual filesystem
      return checkFilesystemContents(a, v);
    }

  } else {
    WARN << "Unsupported reference type: " << ref;
    return false;
  }
}

void Rebuild::setMetadata(shared_ptr<Command> c, shared_ptr<Reference> ref, shared_ptr<Version> v) {
  // Is ref a pipe, access, or something else?
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    WARN << "Warning: Communication through pipes is not yet tracked correctly.";

  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    // The path now resolves to this artifact version
    // TODO: Deal with links, path normalization, etc.
    _entries[a->getPath()] = v;

  } else {
    WARN << "Unsupported reference type: " << ref;
  }
}

void Rebuild::setContents(shared_ptr<Command> c, shared_ptr<Reference> ref, shared_ptr<Version> v) {
  // Is ref a pipe, access, or something else?
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    WARN << "Warning: Communication through pipes is not yet tracked correctly.";

  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    // The path now resolves to this artifact version
    // TODO: Deal with links, path normalization, etc.
    _entries[a->getPath()] = v;

  } else {
    WARN << "Unsupported reference type: " << ref;
  }
}

// Check if an access resolves as-expected in the filesystem
bool Rebuild::checkFilesystemAccess(shared_ptr<Access> ref, int expected) {
  auto path = ref->getPath();
  auto flags = ref->getFlags();

  // If we hit this point, no match was found in any environment. Time to check the filesystem.
  // Set up an access mode that we'll check
  int access_mode = 0;
  if (flags.r) access_mode |= R_OK;
  if (flags.w) access_mode |= W_OK;
  if (flags.x) access_mode |= X_OK;

  // TODO: Is there anything to do for truncate? We need to be sure we can write the file, but is
  // it even possible to open with O_TRUNC in read-only mode?

  // Normally, faccessat checks whether the real user has access. We want to check as whatever the
  // effective user is. That's the same permission level the build would run with.
  int access_flags = AT_EACCESS;

  // Check access on a symlink if nofollow is specified
  if (flags.nofollow) access_flags |= AT_SYMLINK_NOFOLLOW;

  // Use faccessat to check the reference
  int rc = faccessat(AT_FDCWD, path.c_str(), access_mode, access_flags);

  // Check if the access() call failed for some reason
  if (rc) {
    // If the file does not exist, but O_CREAT was included, the access succeeds.
    // Does that match our expected outcome?
    // TODO: Check to be sure we have permission to create the file
    if (errno == ENOENT && flags.create) return expected == SUCCESS;

    // If we hit this point, it's a normal access and errno has the right code.
    // Does the errno value match our expected outcome?
    return expected == errno;
  } else {
    // If the file exists, but O_CREAT and O_EXCL were passed, fail
    if (flags.create && flags.exclusive) return expected == EEXIST;

    // Otherwise, the access succeeds. Does that match the expected outcome?
    return expected == SUCCESS;
  }
}

// Check if the metadata for a file on the actual filesystem matches a saved version
bool Rebuild::checkFilesystemMetadata(shared_ptr<Access> ref, shared_ptr<Version> v) {
  auto ondisk = make_shared<OpenedVersion>(ref);
  ondisk->saveMetadata();
  return v->metadataMatch(ondisk);
}

// Check if the contents of a file on the actual fileystem match a saved version
bool Rebuild::checkFilesystemContents(shared_ptr<Access> ref, shared_ptr<Version> v) {
  auto ondisk = make_shared<OpenedVersion>(ref);
  ondisk->saveFingerprint();
  return v->fingerprintMatch(ondisk);
}
