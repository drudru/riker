#include "core/File.hh"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <capnp/blob.h>
#include <kj/string.h>

#include "core/BuildGraph.hh"
#include "core/Command.hh"
#include "db/db.capnp.h"
#include "fingerprint/blake2.hh"
#include "ui/Graphviz.hh"
#include "ui/log.hh"
#include "ui/options.hh"

using std::make_shared;
using std::ostream;
using std::shared_ptr;

size_t File::next_id = 0;

ostream& operator<<(ostream& o, const File* f) {
  string type = "File";

  if (f->getType() == File::Type::PIPE) {
    type = "Pipe";
  } else if (f->getType() == File::Type::DIRECTORY) {
    type = "Dir";
  }

  o << "[" << type;
  if (f->getPath() != "") o << " " << f->getPath();
  o << "]";
  return o;
}

ostream& operator<<(ostream& o, const File::Version* v) {
  return o << v->getFile() << "@" << v->getIndex();
}

void File::createdBy(Command* c) {
  // Tag a new created version
  auto v = makeVersion(Version::Action::CREATE, c);

  // Record the output edge from the command
  if (c->addOutput(v)) INFO << v << " created by " << c;
}

void File::readBy(Command* c) {
  // If this file has no previous versions, tag a version that references an existing file
  if (_versions.size() == 0) {
    // A reference version has no creator
    makeVersion(Version::Action::REFERENCE);
  }

  // Record the dependency
  if (c->addInput(&_versions.back())) INFO << c << " read " << &_versions.back();
}

void File::mayWrite(Command* c) {
  // TODO
}

void File::writtenBy(Command* c) {
  // There must be a previous version if we're writing a file. If the first action performed on a
  // file is to write to it, there will be a create, reference, or truncate version already.
  FAIL_IF(_versions.size() == 0) << "Invalid write to file with no prior version: " << _path;

  // If the previous version was a write by this command, we don't need to tag a new version
  if (_versions.back()._action == Version::Action::WRITE && _versions.back()._writer == c) {
    return;
  }

  // Otherwise we tag a new written version
  auto v = makeVersion(Version::Action::WRITE, c);

  // Record the output edge
  if (c->addOutput(v)) INFO << c << " wrote " << v;
}

void File::mayTruncate(Command* c) {
  // TODO
}

void File::truncatedBy(Command* c) {
  // Tag a truncated version
  auto v = makeVersion(Version::Action::TRUNCATE, c);

  // Record the output edge
  if (c->addOutput(v)) INFO << c << " truncated " << v;
}

void File::mayDelete(Command* c) {
  // TODO
}

void File::deletedBy(Command* c) {
  // Tag a deleted version
  auto v = makeVersion(Version::Action::DELETE, c);

  // Record the output edge
  if (c->addOutput(v)) INFO << c << " deleted " << v;
}

void File::serialize(Serializer& serializer, db::File::Builder builder) {}

File::Version* File::makeVersion(Version::Action a, Command* c) {
  if (_versions.size() > 0) _versions.back().fingerprint();
  _versions.push_back(Version(this, _versions.size(), a, c));

  if (_versions.size() == 1) {
    _versions.back().fingerprint();
    if (_versions.back()._has_metadata && _type == Type::UNKNOWN) {
      switch (_versions.back()._metadata.st_mode & S_IFMT) {
        case S_IFDIR:
          _type = Type::DIRECTORY;
          break;

        case S_IFIFO:
          _type = Type::PIPE;
          break;

        case S_IFLNK:
          _type = Type::SYMLINK;
          break;

        default:
          _type = Type::REGULAR;
          break;
      }
    }
  }

  return &_versions.back();
}

void File::drawGraph(Graphviz& g) {
  if (_versions.size() == 1) {
    g.addNode(&_versions.front(), true);
  } else {
    g.startSubgraph(this);
    for (auto& v : _versions) {
      g.addNode(&v);
    }
  
    File::Version* prev = nullptr;
    for (auto& v : _versions) {
      if (prev != nullptr) {
        g.addEdge(prev, &v);
      }
      prev = &v;
    }
  
    g.finishSubgraph();
  }
}

void File::Version::fingerprint() {
  if (_has_fingerprint) return;
  if (_file->getType() == File::Type::PIPE) return;

  if (stat(_file->getPath().c_str(), &_metadata) == 0) {
    _has_metadata = true;
  } else {
    WARN << "Unable to stat file " << _file->getPath();
  }
}
