#include "DirArtifact.hh"

#include <list>
#include <memory>
#include <string>
#include <utility>

#include <dirent.h>
#include <sys/types.h>

#include "build/Build.hh"
#include "build/Env.hh"
#include "build/Resolution.hh"
#include "core/IR.hh"
#include "util/log.hh"

using std::list;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::tie;

DirArtifact::DirArtifact(Env& env,
                         shared_ptr<MetadataVersion> mv,
                         shared_ptr<DirVersion> dv) noexcept :
    Artifact(env, mv) {
  _dir_versions.push_front(dv);
  appendVersion(dv);
}

bool DirArtifact::isSaved() const noexcept {
  // TODO: Make sure all dir versions are committed or saved
  return Artifact::isSaved();
}

bool DirArtifact::isCommitted() const noexcept {
  for (auto v : _dir_versions) {
    if (!v->isCommitted()) return false;
  }
  return Artifact::isCommitted();
}

void DirArtifact::commit(shared_ptr<Reference> ref) noexcept {
  // Commit each version, working from the oldest to newest
  for (auto iter = _dir_versions.rbegin(); iter != _dir_versions.rend(); iter++) {
    auto v = *iter;
    v->commit(ref);
  }
}

void DirArtifact::finalize(shared_ptr<Reference> ref, bool commit) noexcept {
  // If we've been here before, don't finalize the directory again (symlinks can create cycles)
  if (_finalized) return;
  _finalized = true;

  // Coerce the reference to one that has a path
  auto a = ref->as<Access>();
  ASSERT(a) << "Somehow a directory was reached without a path";

  // Walk through and finalize each directory entry
  for (auto& [name, wp] : _resolved) {
    auto artifact = wp.lock();
    if (name == "." || name == "..") continue;
    if (artifact) artifact->finalize(make_shared<Access>(a, name, AccessFlags{}), commit);
  }

  // If requested, commit all final state to the filesystem
  if (commit) this->commit(ref);

  // Allow the artifact to finalize metadata
  Artifact::finalize(ref, commit);
}

void DirArtifact::needsCurrentVersions(shared_ptr<Command> c) noexcept {
  // Create dependencies on all the uncommitted versions
  for (auto& v : _dir_versions) {
    _env.getBuild().observeInput(c, shared_from_this(), v, InputType::Inherited);
  }

  // Forward the call to Artifact to create a dependency on metadata
  Artifact::needsCurrentVersions(c);
}

Resolution DirArtifact::getEntry(shared_ptr<Command> c,
                                 shared_ptr<Reference> ref,
                                 string entry) noexcept {
  // If we're looking for ".", return immediately
  if (entry == ".") return shared_from_this();

  auto access = ref->as<Access>();
  ASSERT(access) << "Program somehow reached a directory without a path";

  // Loop through versions until we get a definite answer about the entry
  Lookup found = Lookup::Maybe;
  shared_ptr<DirVersion> matched;

  // First check the uncommitted versions
  for (auto& v : _dir_versions) {
    found = v->hasEntry(_env, access, entry);
    if (found != Lookup::Maybe) {
      matched = v;
      break;
    }
  }

  // Make sure we have a definite result
  ASSERT(found != Lookup::Maybe) << "Directory lookup concluded without a definite answer";

  if (found == Lookup::Yes) {
    // Record the dependency on the matching version
    _env.getBuild().observeInput(c, shared_from_this(), matched, InputType::PathResolution);

    // Look in the cache of resolved artifacts
    auto iter = _resolved.find(entry);
    if (iter != _resolved.end()) {
      // Found a match. Return it now.
      return iter->second.lock();
    } else {
      // No cached artifact. Ask the version to provide the artifact if it can
      auto artifact = matched->getEntry(entry);

      // If the version did not provide an artifact, look in the environment
      if (!artifact) {
        artifact = _env.getPath(access->getFullPath() / entry);
        ASSERT(artifact) << "Failed to locate artifact for existing entry " << entry << " in "
                         << ref;
      }

      // Save the resolved artifact in the cache and return
      _resolved.emplace_hint(iter, entry, artifact);
      return artifact;
    }

  } else {
    // The entry does not exist. Record a dependency on the version that excluded this entry.
    _env.getBuild().observeInput(c, shared_from_this(), matched, InputType::PathResolution);
    return ENOENT;
  }
}

// Apply a link version to this artifact
void DirArtifact::apply(shared_ptr<Command> c,
                        shared_ptr<Reference> ref,
                        shared_ptr<LinkVersion> writing) noexcept {
  // TODO: If this link is only possible because of some earlier version, add input edges.

  // Notify the build of this output
  _env.getBuild().observeOutput(c, shared_from_this(), writing);

  // Add the version to the sequence of directory versions
  _dir_versions.push_front(writing);

  // Record this version in the artifact as well
  appendVersion(writing);

  // Cache the resolution for this linked artifact
  _resolved[writing->getEntryName()] = writing->getTarget()->getArtifact();
}

// Apply an unlink version to this artifact
void DirArtifact::apply(shared_ptr<Command> c,
                        shared_ptr<Reference> ref,
                        shared_ptr<UnlinkVersion> writing) noexcept {
  // TODO: If this unlink is only possible because of some earlier version, add input edges.

  // Notify the build of this output
  _env.getBuild().observeOutput(c, shared_from_this(), writing);

  // Add the version to the sequence of directory versions
  _dir_versions.push_front(writing);

  // Record this version in the artifact as well
  appendVersion(writing);

  // Remove the unlinked entry from the cache of resolved artifacts
  _resolved.erase(writing->getEntryName());
}
