#include <filesystem>
#include <iostream>
#include <string.h>
#include <unistd.h>

#include "util/options.hh"

<<<<<<< HEAD
int main(int argc, char* argv[]) {
	// Can also use /usr/bin/ssh if slogin is not available
	// std::string commandbuild = "slogin";
	
	std::cout << "\n";
	//printf("argc: %d\n", argc);
	
	char* command[argc + 10];  
	command[0] = strdup("slogin");
	int index = 1;
	
	// std::cout << "The command is following: " << command[0] << "\n"; 
	
	for (; index < argc - 1; index++)  
	{  
		command[index] = strdup(argv[index]);
	}  		
  	
  	
  	/*
	// right now it only accept only one command line argument
	for (int i = 1; i < argc - 1; ++i) 
		commandbuild = commandbuild + " " + argv[i];
	*/
	
	
	command[index] = strdup(getenv("RKR_REMOTE_PATH"));
	strcat(command[index], strdup("/src/ssh-wrapper/\\remote-trace"));
	
	// printf("%s\n", argv[argc - 1]);
	
	command[index + 1] = strdup(argv[argc - 1]);
	
	
	command[index + 2] = NULL;
	
	std::cout << "This is the command: ";
	for (int i = 0; i < index + 2; i++) {
		std::cout << command[i] << " "; 
	}
	std::cout << "\n";
=======
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  std::cout << "Current path is " << fs::current_path() << '\n';  // (1)
  // Can also use /usr/bin/ssh if slogin is not available
  std::string commandbuild = "slogin";

  // right now it only accept only one command line argument
  for (int i = 1; i < argc - 1; ++i) commandbuild = commandbuild + " " + argv[i];
>>>>>>> 907ceb228d27231ff6fa1900794ccf8a8190e015

	
	execvp("slogin", command);
	
	
	
	
	/*
	commandbuild = commandbuild + " " + getenv("RKR_REMOTE_PATH") + "/src/ssh-wrapper/\\remote-trace";
	

	commandbuild = commandbuild + " " + argv[argc - 1];
	const char* command = commandbuild.c_str();
	system(command);
	*/

<<<<<<< HEAD
	return 0;
}
=======
  return 0;
}
>>>>>>> 907ceb228d27231ff6fa1900794ccf8a8190e015
