#pragma once

#include <string>
#include <GLES3/gl3.h>

#include "noncopyable.hpp"

class shaders : public noncopyable {
public:
	shaders();
	~shaders();

	bool init(const std::string &vertex_src,
		  const std::string &fragment_src);
	bool load(const std::string &vertex_file,
		  const std::string &fragment_file);

	void use() const;
	GLint location(const std::string &name) const;
private:
	GLuint prog_;
};
