#include <cstdlib>
#include <iostream> 

// to disprove discord theory 

int main() { 
    auto str = std::string("hi") ; 
    auto str_c_style = str.c_str();

    std::cout << "length: " << sizeof(str_c_style) << " " << str.length() << " " << str.size(); 
    return EXIT_SUCCESS;
}