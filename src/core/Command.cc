#include "Command.hh"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Artifact.hh"
#include "core/IR.hh"
#include "tracing/Tracer.hh"
#include "ui/Graphviz.hh"
#include "ui/log.hh"

using std::dynamic_pointer_cast;
using std::map;
using std::shared_ptr;
using std::string;
using std::vector;

string Command::getShortName() const {
  auto base = _exe;
  if (_args.size() > 0) base = _args.front();

  auto pos = base.rfind('/');
  if (pos == string::npos) {
    return base;
  } else {
    return base.substr(pos + 1);
  }
}

string Command::getFullName() const {
  string result;
  for (const string& arg : _args) {
    result += arg + " ";
  }
  return result;
}

void Command::run(Tracer& tracer) {
  // TODO: checking logic goes here
  // simulate all of the steps:
  //   references: check that access remains the same
  //   predicates: still hold
  //   action: simulate effect of actions
  //     two things to check:
  //     1. whether the action had the same effect as before
  //     2. if the action had the same effect, what the effect actually is
  //     NOTE: use recursive state environment

  // We are rerunning this command, so clear the list of steps
  _steps.clear();

  // Actually run the command
  tracer.run(shared_from_this());
}

/// The command accesses an artifact by path.
shared_ptr<Reference> Command::access(string path, Reference::Access::Flags flags) {
  auto ref = make_shared<Reference::Access>(path, flags);
  _steps.push_back(ref);

  // TODO: if f exists and O_TRUNC is set in flags, this access creates a new version of the file
  // TODO: if f does not exist and O_CREAT is set, this access adds an entry to the containing
  // directory

  return ref;
}

/// This command creates a reference to a new pipe
shared_ptr<Reference> Command::pipe() {
  auto ref = make_shared<Reference::Pipe>();
  _steps.push_back(ref);
  return ref;
}

/// This command requires that a reference resolves to an artifact without failure
void Command::isOK(shared_ptr<Reference> ref) {
  _steps.push_back(make_shared<Predicate::IsOK>(ref));
}

/// This command requires that a reference fails to resolve with a specific error
void Command::isError(shared_ptr<Reference> ref, int err) {
  _steps.push_back(make_shared<Predicate::IsError>(ref, err));
}

/// This command accesses the metadata for an artifact
void Command::metadataMatch(shared_ptr<Reference> ref, shared_ptr<Artifact> a) {
  // Get the version we depend on
  auto v = a->getLatestVersion();

  // Make sure we have metadata saved for that version
  v.saveMetadata();

  // Record the dependency on metadata
  _steps.push_back(make_shared<Predicate::MetadataMatch>(ref, a->getLatestVersion()));
}

/// This command accesses the contents of an artifact
void Command::contentsMatch(shared_ptr<Reference> ref, shared_ptr<Artifact> a) {
  _steps.push_back(make_shared<Predicate::ContentsMatch>(ref, a->getLatestVersion()));
}

/// This command sets the metadata for an artifact
void Command::setMetadata(shared_ptr<Reference> ref, shared_ptr<Artifact> a) {
  _steps.push_back(make_shared<Action::SetContents>(ref, a->tagNewVersion()));
}

/// This command sets the contents of an artifact
void Command::setContents(shared_ptr<Reference> ref, shared_ptr<Artifact> a) {
  _steps.push_back(make_shared<Action::SetContents>(ref, a->tagNewVersion()));
}

/// This command launches a child command
void Command::launch(shared_ptr<Command> cmd) {
  _steps.push_back(make_shared<Action::Launch>(cmd));
}

void Command::drawGraph(Graphviz& g) {
  g.addCommand(shared_from_this());
  for (auto& s : _steps) {
    shared_ptr<Predicate::MetadataMatch> metadata_match;
    shared_ptr<Predicate::ContentsMatch> contents_match;
    shared_ptr<Action::Launch> launch;
    shared_ptr<Action::SetMetadata> set_metadata;
    shared_ptr<Action::SetContents> set_contents;

    if ((metadata_match = dynamic_pointer_cast<Predicate::MetadataMatch>(s))) {
      auto version = metadata_match->getVersion();
      g.addInputEdge(version, shared_from_this());

    } else if ((contents_match = dynamic_pointer_cast<Predicate::ContentsMatch>(s))) {
      auto version = contents_match->getVersion();
      g.addInputEdge(version, shared_from_this());

    } else if ((launch = dynamic_pointer_cast<Action::Launch>(s))) {
      // Recursively draw the child command
      launch->getCommand()->drawGraph(g);
      // Add a graph edge to the child command
      g.addCommandEdge(shared_from_this(), launch->getCommand());

    } else if ((set_metadata = dynamic_pointer_cast<Action::SetMetadata>(s))) {
      auto version = set_metadata->getVersion();
      g.addOutputEdge(shared_from_this(), version);

    } else if ((set_contents = dynamic_pointer_cast<Action::SetContents>(s))) {
      auto version = set_contents->getVersion();
      g.addOutputEdge(shared_from_this(), version);
    }
  }
}
