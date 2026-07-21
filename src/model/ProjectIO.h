#pragma once

#include <string>

namespace hopline {

class Project;

// Project persistence. JSON text so files are human-readable and diffable, and
// the model stays headless (no Qt). Media is referenced by path, not embedded.
std::string serializeProject(const Project& project);
bool deserializeProject(const std::string& json, Project& out, std::string& error);

bool saveProject(const Project& project, const std::string& path, std::string& error);
bool loadProject(const std::string& path, Project& out, std::string& error);

}  // namespace hopline
