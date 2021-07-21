#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "artifacts/Artifact.hh"
#include "data/InputTrace.hh"
#include "runtime/Build.hh"
#include "runtime/env.hh"
#include "util/constants.hh"
#include <bits/stdc++.h>

using std::array;
using std::cerr;
using std::cout;
using std::endl;
using std::ifstream;
using std::lock_guard;
using std::make_shared;
using std::mutex;
using std::nullopt;
using std::ofstream;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

// class wrapper for output file for synchronized writing among threads
class SynchronizedFile {
 public:
  SynchronizedFile(const string& path) : _path(path) { myfile.open(path); }

  ~SynchronizedFile() { myfile.close(); }

  // Write to the file
  void write(const string& dataToWrite) {
    lock_guard<mutex> lock(_writerMutex);
    myfile << dataToWrite << endl;
  }

  // Add a package dependency and write to the file
  void addPackage(const string& package) {
    lock_guard<mutex> lock(_writerMutex);
    if (find(packages.begin(), packages.end(), package) == packages.end()) {
      packages.push_back(package);
      myfile << package << endl;
    }
  }

 private:
  string _path;
  ofstream myfile;
  mutex _writerMutex;
  vector<string> packages;
};

// Given the path to a file, return the pacakge managing that file
string getPackage(string path) {
  // Shell command that searches for the package
  string command = "dpkg -S " + path + " 2>&1";

  // Open a pipe that redirect the output of the command
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    exit(EXIT_FAILURE);
  }

  // Read the output of the command
  array<char, 128> buffer;
  string result;
  while (fgets(buffer.data(), buffer.size(), pipe) != NULL) {
    result += buffer.data();
  }
  pclose(pipe);

  return result;
}

// Arguments for each child thread include pointer to the path of file and pointer to the output
// file
struct thread_data {
  // string path_arg;
  shared_ptr<Artifact> path_ptr;
  shared_ptr<SynchronizedFile> file_ptr;
};

// Each thread check one artifact and try to find its package
void* myThread(void* threadarg) {
  // Grab the arguments
  struct thread_data* my_data;
  my_data = (struct thread_data*)threadarg;
  // auto path = my_data->path_arg;
  auto file = my_data->file_ptr;
  auto a = my_data->path_ptr;

  // If a is a null pointer, then skip
  if (!a) pthread_exit(NULL);
  // Skip files without committed path
  if (a->getCommittedPath() == nullopt) pthread_exit(NULL);
  // If a is an anonymous artifact, skip
  if (a->getName().empty()) pthread_exit(NULL);
  // If a is a directory, skip
  if (a->getTypeName() == "Dir") pthread_exit(NULL);
  // If a is special, skip
  if (a->getTypeName() == "Special") pthread_exit(NULL);
  // Remove Symlink
  // if (a->getTypeName() == "Symlink") pthread_exit(NULL);

  // Get the path of that artifact
  string path = a->getCommittedPath().value().string();
  // Remove all files in current directory
  if (path.rfind(get_current_dir_name()) == 0) pthread_exit(NULL);
  // Remove Riker related files
  if (path.find("riker") != string::npos) pthread_exit(NULL);
  // Remove Rikerfile
  if (path == "Rikerfile") pthread_exit(NULL);
  // Remove /proc directory
  if (path.rfind("/proc/") == 0) pthread_exit(NULL);
  // Remove .gitconfig
  if (path.find(".gitconfig") != string::npos) pthread_exit(NULL);

  // cout << path << endl;

  // if (path.find("include") == string::npos) {
  //   if (path.find("/usr/") != string::npos) {
  //     path.erase(0, 4);
  //   }
  //   path = "'*" + path + "'";
  // }
  // file->write(a->getTypeName() + ": " + path);
  // myfile << a->getTypeName() << ": " << path << endl;

  // Get the package for the artifact
  string result = getPackage(path);
  if (result.find("no path found") != string::npos) {
    // At least three different leaf directories: bin, lib
    // Different possible root directories: /, /usr/, /usr/local/

    // Account for the situation where dpkg needs specific path but a hard link is provided
    if (path.rfind("/bin/") == 0 || path.rfind("/lib/") == 0 || path.rfind("/usr/") == 0) {
      string alternative1, alternative2;
      // Store the original file in alternative
      if (path.find("/usr/") == string::npos) {
        alternative1 = "/usr" + path;
        alternative2 = "/usr/local" + path;
      } else if (path.rfind("/usr/local") == 0) {
        alternative1 = path.substr(10, string::npos);
        alternative2 = "/usr" + alternative1;
      } else {
        alternative1 = path.substr(4, string::npos);
        alternative2 = "/usr/local" + alternative1;
      }
      // fd_original = open(path.c_str(), O_RDONLY);
      // fd_alt = open(alternative.c_str(), O_RDONLY);
      // // Check if the alternative exists
      // if (fd_original < 0) {
      //   perror("Error opening the files");
      // }
      // if (fd_alt < 0) {
      //   cout << alternative << " doesn't exist" << endl;
      //   pthread_exit(NULL);
      // }
      // Check the device and inode number to make sure alternative and path are the same file
      struct stat file_stat_original, file_stat_alt1, file_stat_alt2;
      int ret_original, ret_alt1, ret_alt2;
      ret_original = stat(path.c_str(), &file_stat_original);
      ret_alt1 = stat(alternative1.c_str(), &file_stat_alt1);
      ret_alt2 = stat(alternative2.c_str(), &file_stat_alt2);
      if (ret_original < 0) {
        perror("Error getting original file stat");
        pthread_exit(NULL);
      }
      if (ret_alt1 < 0) {
        if (ret_alt2 < 0) {
          perror("Alternative files don't exist");
          pthread_exit(NULL);
        } else {
          // If they are the same file, get the package of alternative
          if (file_stat_original.st_ino == file_stat_alt2.st_ino &&
              file_stat_original.st_dev == file_stat_alt2.st_dev) {
            result = getPackage(alternative2);
          }
        }
      } else {
        if (ret_alt2 < 0) {
          if (file_stat_original.st_ino == file_stat_alt1.st_ino &&
              file_stat_original.st_dev == file_stat_alt1.st_dev) {
            result = getPackage(alternative1);
          }
        } else {
          if (file_stat_original.st_ino == file_stat_alt1.st_ino &&
              file_stat_original.st_dev == file_stat_alt1.st_dev) {
            result = getPackage(alternative1);
          }
          if (file_stat_original.st_ino == file_stat_alt2.st_ino &&
              file_stat_original.st_dev == file_stat_alt2.st_dev) {
            result = getPackage(alternative2);
          }
        }
      }
    }
    if (result.find("no path found") != string::npos) {
      cout << "No path found for " << path << endl;
      cout << a->getName() << endl;
      // file->write("No path found for " + path);
      pthread_exit(NULL);
    }
  }
  // file->write(result);
  // myfile << result << endl;

  // Clean up the package name and append it to the output file
  string package = result.substr(0, result.find(" "));
  package.pop_back();
  // if (find(packages.begin(), packages.end(), package) == packages.end()) {
  //   packages.push_back(package);
  //   file->write(package);
  // myfile << package << endl;
  // }
  // if (package == "dpkg-query") cout << path << endl;
  file->addPackage(package);

  pthread_exit(NULL);
}

/**
 * Run the 'gen_deps' subcommand
 */
void do_gen_deps(vector<string> args) noexcept {
  // Turn on input/output tracking
  // options::track_inputs_outputs = true;

  // Load the serialized build trace
  auto [root_cmd, trace] = InputTrace::load(constants::DatabaseFilename, args);

  // Emulate the trace
  trace->sendTo(Build());

  // ofstream myfile;
  // myfile.open(".rkr-deps");
  auto synchronizedFile = make_shared<SynchronizedFile>(".rkr-deps");

  // list of packages needed
  // vector<string> packages;

  int num_threads = env::getArtifacts().size();
  pthread_t threads[num_threads];
  struct thread_data tds[num_threads];
  int rc, i = 0;

  // Iterate through each artifact
  for (const auto& weak_artifact : env::getArtifacts()) {
    auto a = weak_artifact.lock();

    // cout << a->getTypeName() << ": " << a->getName() << endl;

    // cout << "test" << endl;
    // Initilize thread arguments
    tds[i].path_ptr = a;
    tds[i].file_ptr = synchronizedFile;
    // struct thread_data td = {a, synchronizedFile};
    // pthread_t pid;
    rc = pthread_create(&threads[i], NULL, myThread, (void*)&tds[i]);

    if (rc) {
      cout << "Error: unable to create thread," << rc << endl;
      exit(-1);
    }
    i++;
  }

  void* status;

  // Join all the threads
  for (i = 0; i < num_threads; i++) {
    rc = pthread_join(threads[i], &status);
    if (rc) {
      cout << "Error: unable to join," << rc << endl;
      exit(-1);
    }
    // cout << "Main: completed thread id:" << i;
    // cout << " exiting with status:" << status << endl;
  }
  // pthread_exit(NULL);
}

void do_install_deps(vector<string> args) noexcept {
  // Open file and check if each package is installed
  array<char, 128> buffer;
  string result, package;
  ifstream myfile(".rkr-deps");
  if (myfile.is_open()) {
    while (getline(myfile, package)) {
      string cmd = "dpkg-query -W " + package;
      unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
      if (!pipe) exit(EXIT_FAILURE);
      while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
      }
      if (result.find("no packages found matching") != string::npos) {
        cout << "Installing" << package << endl;
        cmd = "dpkg -i " + package;
        system(cmd.c_str());
      } else {
        cout << package << " is already installed" << endl;
      }
    }
  }
  myfile.close();
}

void do_check_deps(vector<string> args) noexcept {
  // Open file and print everything
  ifstream f(".rkr-deps");
  if (f.is_open()) {
    cout << f.rdbuf();
  }
}

void do_gen_container(vector<string> args) noexcept {
  ifstream deps(".rkr-deps");
  if (deps.is_open()) {
    if (mkdir(".devcontainer", 0777) == 0) {
      ofstream settings(".devcontainer/devcontainer.json");
      ofstream dockerfile(".devcontainer/Dockerfile");
      // if (!dockerfile.is_open()) std::cerr << "Failed to open file : " << errno << endl;

      settings
          << "{\n  \"name\": \"Container\",\n  \"dockerFile\": \"Dockerfile\",\n  \"settings\": "
             "{\n    \"terminal.integrated.shell.linux\": \"/bin/bash\"\n  },\n  \"remoteUser\": "
             "\"vscode\",\n}"
          << endl;

      dockerfile << "FROM ubuntu:20.04\nARG USERNAME=vscode\nARG USER_UID=1000\nARG "
                    "USER_GID=$USER_UID\nENV DEBIAN_FRONTEND=noninteractive\nRUN apt-get update && "
                    "apt-get -y install --no-install-recommends  \\"
                 << endl;

      string package;

      while (getline(deps, package)) {
        dockerfile << "  " << package << "   \\" << endl;
      }
      dockerfile
          << "  && apt-get autoremove -y  \\\n  && apt-get clean -y \\\n  && rm -rf "
             "/var/lib/apt/lists/*\nRUN touch /usr/bin/docker && chmod +x /usr/bin/docker\nENV "
             "DEBIAN_FRONTEND=dialog"
          << endl;

      dockerfile.close();
      settings.close();
    } else {
      cerr << "Error: " << strerror(errno) << endl;
    }
  } else {
    cout << "Please generate dependencies first" << endl;
  }
  deps.close();
}