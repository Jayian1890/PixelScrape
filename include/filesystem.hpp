/*
 * pixelLib
 * Copyright (c) 2025 Interlaced Pixel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PIXELLIB_CORE_FILESYSTEM_HPP
#define PIXELLIB_CORE_FILESYSTEM_HPP

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif
#define MKDIR(path, mode) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#define GETCWD(buffer, length) _getcwd(buffer, length)
#define CHDIR(path) _chdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/syslimits.h>
#else
#include <limits.h>
#endif
#define MKDIR(path, mode) mkdir(path, mode)
#define RMDIR(path) rmdir(path)
#define GETCWD(buffer, length) getcwd(buffer, length)
#define CHDIR(path) chdir(path)
#endif

namespace pixellib::core::filesystem
{

class FileSystem
{
public:
  static bool exists(const std::string &path)
  {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
  }

  static bool is_directory(const std::string &path)
  {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0)
    {
      return S_ISDIR(buffer.st_mode);
    }
    return false;
  }

  static bool is_regular_file(const std::string &path)
  {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0)
    {
      return S_ISREG(buffer.st_mode);
    }
    return false;
  }

  static std::string read_file(const std::string &path)
  {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  static bool write_file(const std::string &path, const std::string &content)
  {
    std::ofstream file(path, std::ios::binary);
    if (file.is_open())
    {
      file << content;
      file.close();
      return true;
    }
    return false;
  }

  static bool create_directory(const std::string &path)
  {
    return MKDIR(path.c_str(), 0777) == 0;
  }

  static bool create_directories(const std::string &path)
  {
    if (path.empty())
      return false;

    if (path == "/" || path == "\\" || (path.length() == 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')))
    {
      return true;
    }

    if (is_directory(path))
    {
      return true;
    }

    std::string parent = path;

    while (!parent.empty() && (parent.back() == '/' || parent.back() == '\\'))
    {
      parent.pop_back();
    }

    size_t pos = parent.find_last_of("/\\\\");
    if (pos != std::string::npos)
    {
      std::string parent_dir = parent.substr(0, pos);
      if (!parent_dir.empty() && !is_directory(parent_dir))
      {
        if (!create_directories(parent_dir))
        {
          return false;
        }
      }
    }

    return create_directory(path);
  }

  static bool remove(const std::string &path)
  {
    if (is_directory(path))
    {
      return RMDIR(path.c_str()) == 0;
    }
    else
    {
      return ::unlink(path.c_str()) == 0;
    }
  }

  static bool copy_file(const std::string &source, const std::string &destination)
  {
    std::ifstream src(source, std::ios::binary);
    std::ofstream dst(destination, std::ios::binary);

    if (!src.is_open() || !dst.is_open())
    {
      return false;
    }

    dst << src.rdbuf();
    return src.good() && dst.good();
  }

  static bool rename(const std::string &source, const std::string &destination)
  {
    return ::rename(source.c_str(), destination.c_str()) == 0;
  }

  static long long file_size(const std::string &path)
  {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0)
    {
      return static_cast<long long>(buffer.st_size);
    }
    return -1;
  }

  static std::time_t last_write_time(const std::string &path)
  {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) == 0)
    {
      return buffer.st_mtime;
    }
    return -1;
  }

  static std::vector<std::string> directory_iterator(const std::string &path)
  {
    std::vector<std::string> result;
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA((path + "\\\\*").c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      do
      {
        std::string filename(findData.cFileName);
        if (filename != "." && filename != "..")
        {
          result.push_back(filename);
        }
      } while (FindNextFileA(hFind, &findData));
      FindClose(hFind);
    }
#else
    DIR *dir = opendir(path.c_str());
    if (dir)
    {
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr)
      {
        std::string filename(entry->d_name);
        if (filename != "." && filename != "..")
        {
          result.push_back(filename);
        }
      }
      closedir(dir);
    }
#endif

    return result;
  }

  static std::string temp_directory_path()
  {
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetTempPathA(MAX_PATH, buffer);
    return std::string(buffer);
#else
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir)
    {
      return std::string(tmpdir);
    }
    return "/tmp";
#endif
  }

  static std::string current_path()
  {
#ifdef _WIN32
    char buffer[MAX_PATH];
    if (GETCWD(buffer, MAX_PATH))
    {
      return std::string(buffer);
    }
#else
    char buffer[PATH_MAX];
    if (GETCWD(buffer, PATH_MAX))
    {
      return std::string(buffer);
    }
#endif
    return "";
  }

  static bool current_path(const std::string &path)
  {
    return CHDIR(path.c_str()) == 0;
  }
};

} // namespace pixellib::core::filesystem

#endif
