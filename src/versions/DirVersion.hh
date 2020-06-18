#pragma once

#include <filesystem>
#include <memory>
#include <set>

#include "core/IR.hh"
#include "util/log.hh"
#include "util/serializer.hh"
#include "versions/Version.hh"

using std::set;
using std::shared_ptr;

namespace fs = std::filesystem;

class Env;

/// Possible returned values from an attempt to get an entry from a directory version
enum class Lookup { Yes, No, Maybe };

/**
 * A directory version encodes some or all of the state of a directory. Unlike versions for other
 * artifact types, directory versions can be *partial*. These partial versions encode specific
 * actions like linking or unlinking an entry in a directory.
 */
class DirVersion : public Version {
 public:
  /**
   * Check to see if this directory version guarantees the presence or absence of a named entry.
   * A yes or no answer is definite, but partial versions can return "maybe", indicating that
   * checking should continue on to additional version.
   */
  virtual Lookup hasEntry(Env& env, fs::path dirpath, fs::path name) noexcept = 0;

  /**
   * Get the artifact corresponding to a named entry.
   * Returning nullptr indicates that the directory should get the artifact from the filesystem.
   */
  virtual shared_ptr<Artifact> getEntry(fs::path name) const noexcept { return nullptr; }

 private:
  SERIALIZE_EMPTY();
};

/**
 * A link directory version encodes a single linking operation, which adds an entry to the
 * directory. This is a partial version, so any attempt to resolve entries other than the linked one
 * will fall through to other versions.
 */
class LinkDirVersion : public DirVersion {
 public:
  /// Create a new version of a directory that adds a named entry to the directory
  LinkDirVersion(fs::path entry, shared_ptr<Reference> target) : _entry(entry), _target(target) {}

  /// Get the name for this version type
  virtual string getTypeName() const noexcept override { return "+" + string(_entry); }

  /// Directory links are never saved (at least for now)
  virtual bool isSaved() const noexcept override { return false; }

  /// Do nothing when asked to save this version
  virtual void save(shared_ptr<Reference> ref) noexcept override {}

  /// Do nothing when asked to commit this version
  virtual void commit(shared_ptr<Reference> ref) const noexcept override {}

  /// Directory links are always fingerprinted
  virtual bool hasFingerprint() const noexcept override { return true; }

  /// Do nothing when asked to fingerprint the link
  virtual void fingerprint(shared_ptr<Reference> ref) noexcept override {}

  /// Compare this version to another version
  virtual bool matches(shared_ptr<Version> other) const noexcept override {
    WARN << "Unimplemented directory version comparion!";
    return false;
  }

  virtual Lookup hasEntry(Env& env, fs::path dirpath, fs::path name) noexcept override {
    // If the lookup is searching for the linked entry, return yes. Otherwise fall through.
    if (_entry == name) return Lookup::Yes;
    return Lookup::Maybe;
  }

  virtual shared_ptr<Artifact> getEntry(fs::path name) const noexcept override {
    // If the lookup is searching for the linke entry, return the corresponding artifact.
    if (name == _entry) return _target->getArtifact();

    return nullptr;
  }

 private:
  fs::path _entry;
  shared_ptr<Reference> _target;

  // Create default constructor and declare fields for serialization
  LinkDirVersion() = default;
  SERIALIZE(BASE(DirVersion), _entry, _target);
};

/**
 * An existing directory version is a lazily-populated set of entries that are known to be present
 * or absent. The version looks for entries using a provided environment.
 */
class ExistingDirVersion : public DirVersion {
 public:
  /// Get the name for this version type
  virtual string getTypeName() const noexcept override { return "list"; }

  /// Directory lists are never saved (at least for now)
  virtual bool isSaved() const noexcept override { return false; }

  /// Do nothing when asked to save the directory
  virtual void save(shared_ptr<Reference> ref) noexcept override {}

  /// Do nothing when asked to commit the directory
  virtual void commit(shared_ptr<Reference> ref) const noexcept override {}

  /// Directory lists are always fingerprinted
  virtual bool hasFingerprint() const noexcept override { return true; }

  /// Do nothing when asked to fingerprint the directory
  virtual void fingerprint(shared_ptr<Reference> ref) noexcept override {}

  /// Compare this version to another version
  virtual bool matches(shared_ptr<Version> other) const noexcept override {
    WARN << "Unimplemented directory version comparion!";
    return false;
  }

  /// Check if this version has a specific entry
  virtual Lookup hasEntry(Env& env, fs::path dirpath, fs::path name) noexcept override;

 private:
  /// Entries that are known to be in this directory
  set<fs::path> _present;

  /// Entries that are known NOT to be in this directory
  set<fs::path> _absent;

  // Declare fields for serialization
  SERIALIZE(BASE(DirVersion), _present, _absent);
};

/**
 * A listed directory version is a complete list of the entries in a directory. This can appear as
 * the initial version for a directory that is created during a build. These versions can also be
 * created on-demand when a command lists a directory that has a number of partial versions.
 */
class ListedDirVersion : public DirVersion {
 public:
  /// Create a ListedDirVersion for an initially-empty directory
  ListedDirVersion() {
    // Add placeholder entries for "." and ".."
    _entries.emplace(".");
    _entries.emplace("..");
  }

  /// Get the name for this version type
  virtual string getTypeName() const noexcept override { return "list"; }

  /// Directory lists are never saved (at least for now)
  virtual bool isSaved() const noexcept override { return false; }

  /// Do nothing when asked to save the directory
  virtual void save(shared_ptr<Reference> ref) noexcept override {}

  /// Do nothing when asked to commit the directory
  virtual void commit(shared_ptr<Reference> ref) const noexcept override {}

  /// Directory lists are always fingerprinted
  virtual bool hasFingerprint() const noexcept override { return true; }

  /// Do nothing when asked to fingerprint the directory
  virtual void fingerprint(shared_ptr<Reference> ref) noexcept override {}

  /// Compare this version to another version
  virtual bool matches(shared_ptr<Version> other) const noexcept override {
    WARN << "Unimplemented directory version comparion!";
    return false;
  }

  /// Check if this version has a specific entry
  virtual Lookup hasEntry(Env& env, fs::path dirpath, fs::path name) noexcept override {
    return _entries.find(name) == _entries.end() ? Lookup::No : Lookup::Yes;
  }

 private:
  set<fs::path> _entries;

  // Specify fields for serialization
  SERIALIZE(BASE(DirVersion), _entries);
};
