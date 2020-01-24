#include "gl.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>

shaders::shaders() : prog_(0) {}
shaders::~shaders() { glDeleteProgram(prog_); }

static void print_shader_log(GLuint shader)
{
	int len = 0;
	char *buffer;

	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);

	buffer = new char[len];

	glGetShaderInfoLog(shader, len, &len, buffer);

	std::cerr << "shader logs: " << std::endl << buffer;
	delete [] buffer;
}

static bool compile_shader(GLuint shader, const std::string &source)
{
	GLint status;
	char *src = (char*)source.c_str();

	glShaderSource(shader, 1, &src, 0);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		std::cerr << "glCompileShader: " << std::endl
			  << src << std::endl;
		print_shader_log(shader);
		return false;
	}

	return true;
}

static GLuint attach_shader(GLuint program, GLuint type, const std::string &src)
{
	GLuint shader;

	shader = glCreateShader(type);
	if (!shader) {
		std::cerr << "glCreateShader: " << glGetError() << std::endl;
		return 0;
	}

	if (!compile_shader(shader, src)) {
		glDeleteShader(shader);
		return 0;
	}

	glAttachShader(program, shader);
	return shader;
}

static void clean_shader(GLuint program, GLuint shader)
{
	glDetachShader(program, shader);
	glDeleteShader(shader);
}

static void print_program_log(GLuint prog)
{
	int len = 0;
	char *buffer;

	glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);

	buffer = new char[len];

	glGetProgramInfoLog(prog, len, &len, buffer);

	std::cerr << "program logs: " << std::endl << buffer;
	delete [] buffer;
}

static bool link(GLuint program)
{
	GLint status;

	glLinkProgram(program);

	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		std::cerr << "glLinkProgram: " << std::endl;
		print_program_log(program);
		return false;
	}

	return true;
}

bool shaders::init(const std::string &vertex_src,
		   const std::string &fragment_src)
{
	GLuint vertex = 0, fragment = 0;
	bool ret = false;

	glDeleteProgram(prog_);

	prog_ = glCreateProgram();
	if (!prog_) {
		std::cerr << "glCreateProgram: " << glGetError() << std::endl;
		return false;
	}

	vertex = attach_shader(prog_, GL_VERTEX_SHADER, vertex_src);
	if (!vertex)
		return false;

	fragment = attach_shader(prog_, GL_FRAGMENT_SHADER, fragment_src);
	if (!fragment)
		goto clean_vertex;

	ret = link(prog_);

	clean_shader(prog_, fragment);
clean_vertex:
	clean_shader(prog_, vertex);
	return ret;
}

static std::string read_file(const std::string &file)
{
#ifndef VC_SHADERS_PATH
#warning VC_SHADERS_PATH not defined
#define VC_SHADERS_PATH "."
#endif
	std::string path;
	std::ifstream ifs;
	std::stringstream sstr;
	char *env_path = getenv("VC_SHADERS_PATH");

	path = env_path ? env_path : VC_SHADERS_PATH;

	ifs.open(path + "/" + file);
	if (!ifs) {
		std::cerr << "unable to open: " << file << std::endl;
		return "";
	}

	sstr << ifs.rdbuf();
	return sstr.str();
}

bool shaders::load(const std::string &vertex_file,
		   const std::string &fragment_file)
{
	std::string vertex_src, fragment_src;

	vertex_src   = read_file(vertex_file);
	fragment_src = read_file(fragment_file);

	if (vertex_src.empty() || fragment_src.empty())
		return false;

	return init(vertex_src, fragment_src);
}

void shaders::use() const
{
	glUseProgram(prog_);
}

GLint shaders::location(const std::string &name) const
{
	GLint ret;

	ret = glGetUniformLocation(prog_, name.c_str());
	if (ret == -1)
                ret = glGetAttribLocation(prog_, name.c_str());

	return ret;
}
