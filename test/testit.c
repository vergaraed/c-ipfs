#include "testit.h"
#include "repo/test_repo.h"
#include "repo/test_repo_identity.h"
#include "cmd/ipfs/test_init.h"

int testit(const char* name, int (*func)(void)) {
	printf("Testing %s...\n", name);
	int retVal = func();
	if (retVal)
		printf("%s success!\n", name);
	else
		printf("** Uh oh! %s failed.**\n", name);
	return retVal;
}

int main(int argc, char** argv) {
	testit("config_repo", test_config_repo);
	testit("test_repo_config_identity_new", test_repo_config_identity_new);
	testit("test_repo_config_identity_private_key", test_repo_config_identity_private_key);
	testit("get_init_command", test_get_init_command);
	return 1;
}
