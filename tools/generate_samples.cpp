#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<char> dst_up('A', 'Z');       // 65, 90
std::uniform_int_distribution<char> dst_lwr('a', 'z');      // 97, 122
std::uniform_int_distribution<char> dst_n('0', '9');        // 48, 57

std::string
prepare_string(const int n)
{
  std::string str;
  str.resize(n);
  return str;
}

void
randomize_string(std::string &str)
{
  for ( int i = 0; i < 8; i++ ) str[i] = static_cast<char>(dst_lwr(gen));
  for ( int i = 8; i < 12; i++ ) str[i] = static_cast<char>(dst_n(gen));
}

std::vector<std::string>
make_paths(const std::string &root, const int num)
{
  std::vector<std::string> paths;
  paths.reserve(num);
  for ( int i = 0; i < num; ++i ) {
    paths.push_back(prepare_string(12));
    randomize_string(paths.back());
  }
  return paths;
}

int
main()
{
  const int num_files = 64;
  const std::string root_dir = "sample/";
  auto paths = make_paths(root_dir, num_files);
  for ( auto n : paths ) {
    std::cout << "Generating " << root_dir << n << ".json" << std::endl;
  }
}
