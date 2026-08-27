#include <string>
#include "gitignore.hpp"
// gitignore edit 

void editGitignore(std::string& fc) { 
    if (!fc.contains("gitm.manifest")) { 
        std::string og = fc; 
        fc = og + "\n# gitm manifest \ngitm.manifest "; 
    }
    return;
}

bool containsGitignoreGitm(std::string fc) { return fc.contains("gitm.manifest"); };