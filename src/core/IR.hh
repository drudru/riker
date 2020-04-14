#pragma once

#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <cereal/access.hpp>

#include "core/Artifact.hh"
#include "ui/log.hh"
#include "util/UniqueID.hh"

using std::map;
using std::nullopt;
using std::optional;
using std::ostream;
using std::shared_ptr;
using std::string;

class Reference;

/**
 * A Command's actions are tracked as a sequence of Steps, each corresponding to some operation or
 * dependency we observed the last time a command executed.
 *
 * Step is the abstract parent class for all IR values.
 * All command steps fall into one of three categories:
 * - Reference: a reference to some artifact made by a command
 * - Predicate: a statement about a reference that was true on the example build
 * - Action: a modification to system state performed by the command
 */
class Step {
 public:
  /// Use a default virtual destructor
  virtual ~Step() = default;

  /// Get the unique ID for this IR node
  size_t getID() const { return _id; }

  /**
   * Evaluate this build step in a hypothetical build environment. If the result of this build step
   * is the same as the recorded outcome, return true. Otherwise return false.
   * \param env   A map from paths to artifact versions placed at those paths
   * \returns true if the outcome is unchanged, or false if the build step should be rerun
   */
  virtual bool eval(map<string, ArtifactVersion>& env) = 0;

  /// Check if this step contains a reference
  virtual shared_ptr<Reference> getReference() const { return nullptr; }

  /// Print this Step to an output stream
  virtual ostream& print(ostream& o) const = 0;

  /// Stream print wrapper for Step references
  friend ostream& operator<<(ostream& o, const Step& s) { return s.print(o); }

  /// Stream print wrapper for Step pointers
  friend ostream& operator<<(ostream& o, const Step* s) { return o << *s; }

 private:
  UniqueID<Step> _id;
};

/**
 * A Reference is created any time a command refers to an artifact. This happens when commands open
 * files, but other cases (like creating pipes) will also need to be tracked.
 *
 * The types of references are:
 * - PIPE()
 * - ACCESS(<path>, <mode>)
 */
class Reference : public Step {
 public:
  class Pipe;
  class Access;

  /// Get the path this reference uses, if it has one
  virtual optional<string> getPath() = 0;

  /// References are always successful, and do not need to update the environment
  virtual bool eval(map<string, ArtifactVersion>& env) override { return true; }

  /// Get the result of making this reference again, and return it
  virtual int checkAccess() = 0;

  /// Get the short name for this reference
  string getName() const { return "r" + std::to_string(getID()); }
};

/// Create a reference to a new pipe
class Reference::Pipe : public Reference {
 public:
  virtual ostream& print(ostream& o) const override { return o << getName() << " = PIPE()"; }

  /// Pipes do not have a path
  virtual optional<string> getPath() override { return nullopt; }

  /// Pipes are always created successfully
  virtual int checkAccess() override { return 0; }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, Pipe& p, const uint32_t version);
};

/// Access a filesystem path with a given set of flags
class Reference::Access : public Reference {
  // Default constructor for deserialization
  friend class cereal::access;
  Access() = default;

 public:
  /// This struct encodes the flags specified when making an access to a particular reference
  struct Flags {
    bool r = false;          // Does the reference require read access?
    bool w = false;          // Does the reference require write access?
    bool x = false;          // Does the reference require execute access?
    bool nofollow = false;   // Does the reference resolve to a symlink rather than its target?
    bool truncate = false;   // Does the reference truncate the artifact's contents?
    bool create = false;     // Does the reference create an artifact if none exists?
    bool exclusive = false;  // Does the reference require creation? (must also be set with .create

    /// Create a Flags instance from the flags parameter to the open syscall
    static Flags fromOpen(int flags) {
      return {.r = (flags & O_RDONLY) == O_RDONLY || (flags & O_RDWR) == O_RDWR,
              .w = (flags & O_WRONLY) == O_WRONLY || (flags & O_RDWR) == O_RDWR,
              .nofollow = (flags & O_NOFOLLOW) == O_NOFOLLOW,
              .truncate = (flags & O_TRUNC) == O_TRUNC,
              .create = (flags & O_CREAT) == O_CREAT,
              .exclusive = (flags & O_EXCL) == O_EXCL};
    }

    /// Create a Flags instance from the mode and flags parameters to the access syscall
    static Flags fromAccess(int mode, int flags) {
      return {.r = (mode & R_OK) == R_OK,
              .w = (mode & W_OK) == W_OK,
              .x = (mode & X_OK) == X_OK,
              .nofollow = (flags & AT_SYMLINK_NOFOLLOW) == AT_SYMLINK_NOFOLLOW};
    }

    /// Create a Flags instance from the flags parameter to the stat syscall
    static Flags fromStat(int flags) {
      return {.nofollow = (flags & AT_SYMLINK_NOFOLLOW) == AT_SYMLINK_NOFOLLOW};
    }

    /// Print a Flags struct to an output stream
    friend ostream& operator<<(ostream& o, const Flags& f) {
      return o << (f.r ? 'r' : '-') << (f.w ? 'w' : '-') << (f.x ? 'x' : '-')
               << (f.nofollow ? " nofollow" : "") << (f.truncate ? " truncate" : "")
               << (f.create ? " create" : "") << (f.exclusive ? " exclusive" : "");
    }
  };

  /// Create an access reference to a path with given flags
  Access(string path, Flags flags) : _path(path), _flags(flags) {}

  /// Get the flags used to create this reference
  const Flags& getFlags() const { return _flags; }

  virtual optional<string> getPath() override { return _path; }

  /// Check the outcome of an access
  virtual int checkAccess() override {
    int access_mode = 0;
    if (_flags.r) access_mode |= R_OK;
    if (_flags.w) access_mode |= W_OK;
    if (_flags.x) access_mode |= X_OK;
    // TODO: Support creat, trunc, and excl

    int access_flags = AT_EACCESS;
    if (_flags.nofollow) access_flags |= AT_SYMLINK_NOFOLLOW;

    // Use faccessat to check the reference
    if (faccessat(AT_FDCWD, _path.c_str(), access_mode, access_flags)) {
      // If there's an error, return the error value stored in errno
      return errno;
    } else {
      // If not, return success
      return 0;
    }
  }

  /// Print an access reference
  virtual ostream& print(ostream& o) const override {
    return o << getName() << " = ACCESS(\"" << _path << "\", [" << getFlags() << "])";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, Access& a, const uint32_t version);

 private:
  string _path;  //< The filesystem path that was accessed
  Flags _flags;  //< The relevant flags for the access
};

/**
 * Predicates allow us to encode a command's dependencies. We will check to see whether these
 * predicates still hold true prior to a rebuild; any time a command has at least one failing
 * predicate, we know we have to rerun that command.
 *
 * There are several types of predicates:
 * - IS_OK(r : Reference)
 * - IS_ERROR(r : Reference, e : Error)
 * - METADATA_MATCH(r : Reference, v : ArtifactVersion)
 * - CONTENTS_MATCH(r : Reference, v : ArtifactVersion)
 */
class Predicate : public Step {
 public:
  class IsOK;
  class IsError;
  class MetadataMatch;
  class ContentsMatch;
};

/**
 * Require that a reference was successful (e.g. it did not return an error code)
 */
class Predicate::IsOK : public Predicate {
  // Default constructor for deserialization
  friend class cereal::access;
  IsOK() = default;

 public:
  /// Create an IS_OK predicate
  IsOK(shared_ptr<Reference> ref) : _ref(ref) {}

  /// Get the reference this predicate checks
  virtual shared_ptr<Reference> getReference() const override { return _ref; }

  /// This predicate has changed if a reference is no longer successful
  virtual bool eval(map<string, ArtifactVersion>& env) override { return _ref->checkAccess() == 0; }

  /// Print an IS_OK predicate to an output stream
  virtual ostream& print(ostream& o) const override {
    return o << "IS_OK(" << _ref->getName() << ")";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, IsOK& p, const uint32_t version);

 private:
  shared_ptr<Reference> _ref;  //< The reference that must have been made successfully
};

/**
 * Require that a reference resulted in a specific error code
 */
class Predicate::IsError : public Predicate {
  // Default constructor for deserialization
  friend class cereal::access;
  IsError() = default;

 public:
  /// Create an IS_ERROR predicate
  IsError(shared_ptr<Reference> ref, int err) : _ref(ref), _err(err) {}

  virtual shared_ptr<Reference> getReference() const override { return _ref; }

  /// Return true if the given reference evaluates to a different result
  virtual bool eval(map<string, ArtifactVersion>& env) override {
    int result = _ref->checkAccess();
    if (result != _err) {
      LOG << "Reference returned " << result << " instead of " << _err;
      return false;
    } else {
      return true;
    }
  }

  /// Print an IS_ERROR predicate
  virtual ostream& print(ostream& o) const override {
    // Set up a map from error codes to names
    static map<int, string> errors = {{EACCES, "EACCES"}, {EDQUOT, "EDQUOT"}, {EEXIST, "EEXIST"},
                                      {EINVAL, "EINVAL"}, {EISDIR, "EISDIR"}, {ELOOP, "ELOOP"},
                                      {ENOENT, "ENOENT"}};

    // If we can't identify the error code, just print "EMYSTERY"
    string errname = "EMYSTERY";

    // Look up the error name in our map
    auto iter = errors.find(_err);
    if (iter != errors.end()) {
      errname = iter->second;
    }

    return o << "IS_ERROR(" << _ref->getName() << ", " << errname << ")";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, IsError& p, const uint32_t version);

 private:
  shared_ptr<Reference> _ref;  //< The reference that must have resulted in an error
  int _err;                    //< The error code returned from the reference
};

/**
 * Require that the metadata accessed through a reference matches that of an artifact version
 */
class Predicate::MetadataMatch : public Predicate {
  // Default constructor for deserialization
  friend class cereal::access;
  MetadataMatch() = default;

 public:
  /// Create a METADATA_MATCH predicate
  MetadataMatch(shared_ptr<Reference> ref, ArtifactVersion version) :
      _ref(ref), _version(version) {}

  /// Get the reference used for this predicate
  virtual shared_ptr<Reference> getReference() const override { return _ref; }

  /// Get the expected artifact version
  ArtifactVersion getVersion() const { return _version; }

  /// Check if this predicate has changed
  virtual bool eval(map<string, ArtifactVersion>& env) override {
    optional<string> path = _ref->getPath();

    // References without paths never check out
    if (!path.has_value()) return false;

    // If the environment has this path, we can check the version cached there
    auto iter = env.find(path.value());
    if (iter != env.end()) {
      // This is probably overly-conservative. Any version with the same metadata would be okay.
      return iter->second == _version;
    } else {
      // Check the contents of the referred-to path
      // TODO: handle nofollow flag
      return _version.metadataMatch(path.value());
    }
  }

  /// Print a METADATA_MATCH predicate
  virtual ostream& print(ostream& o) const override {
    return o << "METADATA_MATCH(" << _ref->getName() << ", " << _version << ")";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, MetadataMatch& p, const uint32_t version);

 private:
  shared_ptr<Reference> _ref;  //< The reference being examined
  ArtifactVersion _version;    //< The artifact version whose metadata the reference must match
};

/**
 * Require that the contents accessed through a reference match that of an artifact version
 */
class Predicate::ContentsMatch : public Predicate {
  // Default constructor for deserialization
  friend class cereal::access;
  ContentsMatch() = default;

 public:
  /// Create a CONTENTS_MATCH predicate
  ContentsMatch(shared_ptr<Reference> ref, ArtifactVersion version) :
      _ref(ref), _version(version) {}

  /// Get the reference used for this predicate
  virtual shared_ptr<Reference> getReference() const override { return _ref; }

  /// Get the expected artifact version
  ArtifactVersion getVersion() const { return _version; }

  /// Check if this predicate has changed
  virtual bool eval(map<string, ArtifactVersion>& env) override {
    optional<string> path = _ref->getPath();

    // References without paths never check out
    if (!path.has_value()) return false;

    // If the environment has this path, we can check the version cached there
    auto iter = env.find(path.value());
    if (iter != env.end()) {
      // This is overly-conservative. Any version with the same contents would be okay.
      return iter->second == _version;
    } else {
      // Check the contents of the referred-to path
      // TODO: handle nofollow flag
      return _version.contentsMatch(path.value());
    }
  }

  /// Print a CONTENTS_MATCH predicate
  virtual ostream& print(ostream& o) const override {
    return o << "CONTENTS_MATCH(" << _ref->getName() << ", " << _version << ")";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, ContentsMatch& p, const uint32_t version);

 private:
  shared_ptr<Reference> _ref;  //< The reference being examined
  ArtifactVersion _version;    //< The artifact version whose contents the reference must match
};

/**
 * An action describes a step taken by a command that could become visible to some other command.
 * If we are able to skip execution of a command (all its predicates match) we are responsible for
 * performing these actions on behalf of the skipped command.
 *
 * The types of actions are:
 * - LAUNCH(cmd : Command, inherited_refs : [Reference])
 * - SET_METADATA(r : Reference, v : Artifact::Version)
 * - SET_CONTENTS(r : Reference, v : Artifact::Version)
 */
class Action : public Step {
 public:
  class Launch;
  class SetMetadata;
  class SetContents;
};

/**
 * A Launch action creates a new command, which inherits some (possibly empty)
 * set of references from its parent.
 */
class Action::Launch : public Action {
  // Default constructor for deserialization
  friend class cereal::access;
  Launch() = default;

 public:
  /// Create a LAUNCH action
  Launch(shared_ptr<Command> cmd) : _cmd(cmd) {}

  /// Get the command this action launches
  shared_ptr<Command> getCommand() const { return _cmd; }

  /// A launch action's state is never changed
  virtual bool eval(map<string, ArtifactVersion>& env) override {
    // Launch actions always evaluate successfully
    return true;
  }

  /// Print a LAUNCH action
  virtual ostream& print(ostream& o) const override;

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, Launch& a, const uint32_t version);

 private:
  shared_ptr<Command> _cmd;  //< The command that is being launched
};

/**
 * A SetMetadata action indicates that a command set the metadata for an artifact.
 */
class Action::SetMetadata : public Action {
  // Default constructor for deserialization
  friend class cereal::access;
  SetMetadata() = default;

 public:
  /// Create a SET_METADATA action
  SetMetadata(shared_ptr<Reference> ref, ArtifactVersion version) : _ref(ref), _version(version) {}

  /// Get the reference used for this action
  virtual shared_ptr<Reference> getReference() const override { return _ref; }

  /// Get the artifact version that is put in place
  ArtifactVersion getVersion() const { return _version; }

  /// Check whether this action's outcome has changed
  virtual bool eval(map<string, ArtifactVersion>& env) override {
    optional<string> path = _ref->getPath();

    // If the referred-to artifact doesn't have a path, there's nothing left to do
    if (!path.has_value()) return true;

    // We have a path. Record the effect of this action in the environment
    env[path.value()] = _version;

    // Evaluation succeeds
    return true;
  }

  /// Print a SET_METADATA action
  virtual ostream& print(ostream& o) const override {
    return o << "SET_METADATA(" << _ref->getName() << ", " << _version << ")";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, SetMetadata& a, const uint32_t version);

 private:
  shared_ptr<Reference> _ref;  //< The reference used for this action
  ArtifactVersion _version;    //< The artifact version with the metadata written by this action
};

/**
 * A SetContents action records that a command set the contents of an artifact.
 */
class Action::SetContents : public Action {
  // Default constructor for deserialization
  friend class cereal::access;
  SetContents() = default;

 public:
  /// Create a SET_CONTENTS action
  SetContents(shared_ptr<Reference> ref, ArtifactVersion version) : _ref(ref), _version(version) {}

  /// Get the reference used for this action
  virtual shared_ptr<Reference> getReference() const override { return _ref; }

  /// Get the artifact version that is put in place
  ArtifactVersion getVersion() const { return _version; }

  /// Check whether this action's outcome has changed
  virtual bool eval(map<string, ArtifactVersion>& env) override {
    optional<string> path = _ref->getPath();

    // If the referred-to artifact doesn't have a path, there's nothing left to do
    if (!path.has_value()) return true;

    // We have a path. Record the effect of this action in the environment
    env[path.value()] = _version;

    // Evaluation succeeds
    return true;
  }

  /// Print a SET_CONTENTS action
  virtual ostream& print(ostream& o) const override {
    return o << "SET_CONTENTS(" << _ref->getName() << ", " << _version << ")";
  }

  /// Friend method for serialization
  template <class Archive>
  friend void serialize(Archive& archive, SetContents& a, const uint32_t version);

 private:
  shared_ptr<Reference> _ref;  //< The reference used for this action
  ArtifactVersion _version;    //< The artifact version with the contents written by this action
};
