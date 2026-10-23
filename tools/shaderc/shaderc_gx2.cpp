/*
 * Copyright 2026. License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include "shaderc.h"
#include "../../src/shader.h"

#if SHADERC_CONFIG_HAS_CAFEGLSL
#include <cafeglsl/CafeGLSLCompiler.h>
#include <cafeglsl/gfd.h>
#include <memory>
#include <regex>

namespace bgfx
{
	namespace
	{
		struct CompilerSession
		{
			CompilerSession() { InitGLSLCompiler(); }
			~CompilerSession() { DestroyGLSLCompiler(); }
		};

		bool samplerMetadata(Uniform& _uniform, const std::string& _type)
		{
			const bool integer = _type[0] == 'i';
			const bool unsignedInteger = _type[0] == 'u';
			const bool shadow = _type.find("Shadow") != std::string::npos;
			_uniform.type = UniformType::Enum(UniformType::Sampler | kUniformSamplerBit | (shadow ? kUniformCompareBit : 0));
			_uniform.texComponent = textureComponentTypeToId(integer ? TextureComponentType::Int
				: unsignedInteger ? TextureComponentType::Uint : shadow ? TextureComponentType::Depth : TextureComponentType::Float);
			const bool array = _type.find("Array") != std::string::npos;
			const bool multisample = _type.find("MS") != std::string::npos;
			TextureDimension::Enum dimension;
			if (_type.find("2D") != std::string::npos)
			{
				dimension = multisample ? (array ? TextureDimension::Dimension2DMSArray : TextureDimension::Dimension2DMS)
					: (array ? TextureDimension::Dimension2DArray : TextureDimension::Dimension2D);
			}
			else if (_type.find("Cube") != std::string::npos)
			{
				dimension = array ? TextureDimension::DimensionCubeArray : TextureDimension::DimensionCube;
			}
			else if (_type.find("3D") != std::string::npos)
			{
				dimension = TextureDimension::Dimension3D;
			}
			else if (_type.find("1D") != std::string::npos && !array)
			{
				dimension = TextureDimension::Dimension1D;
			}
			else
			{
				return false;
			}
			_uniform.texDimension = textureDimensionToId(dimension);
			return true;
		}

		struct SamplerDeclaration
		{
			std::string type;
			uint32_t count;
		};

		typedef std::unordered_map<std::string, SamplerDeclaration> SamplerDeclarations;

		bool samplerDeclarations(const std::string& _code, SamplerDeclarations& _result, bx::WriterI* _messages)
		{
			const std::regex declaration(R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?([iu]?sampler[A-Za-z0-9]+)\s+([^;]+);)");
			const std::regex declarator(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*((?:\[[^\]]*\]\s*)*)$)");
			const std::regex extent(R"(^\[\s*(0[xX][0-9a-fA-F]+|[0-9]+)[uU]?\s*\]\s*$)");
			for (std::sregex_iterator it(_code.begin(), _code.end(), declaration), end; it != end; ++it)
			{
				const std::string type = (*it)[1].str();
				const std::string names = (*it)[2].str();
				for (size_t begin = 0; begin < names.size(); )
				{
					const size_t comma = names.find(',', begin);
					const std::string name = names.substr(begin, comma == std::string::npos ? comma : comma - begin);
					std::smatch match;
					if (std::regex_match(name, match, declarator))
					{
						uint32_t count = 1;
						const std::string dimensions = match[2].str();
						if (dimensions.find('[', 1) != std::string::npos)
						{
							bx::ErrorAssert err;
							bx::write(_messages, &err, "GX2: multidimensional sampler %s lacks complete compiler reflection; use a one-dimensional sampler array.\n", match[1].str().c_str());
							return false;
						}
						if (!dimensions.empty())
						{
							std::smatch dimension;
							const unsigned long size = std::regex_match(dimensions, dimension, extent)
								? strtoul(dimension[1].str().c_str(), NULL, 0) : 0;
							count = size <= 16 ? uint32_t(size) : 0;
						}
						_result[match[1].str()] = { type, count };
					}
					if (comma == std::string::npos) break;
					begin = comma + 1;
				}
			}
			return true;
		}

		template<typename Shader>
		bool writeStage(const Options& _options, const Shader& _shader, const std::string& _code,
			const std::vector<uint8_t>& _gfd, bx::WriterI* _writer, bx::WriterI* _messages)
		{
			bx::ErrorAssert err;
			UniformArray uniforms;
			for (uint32_t ii = 0; ii < _shader.uniformVarCount; ++ii)
			{
				const GX2UniformVar& var = _shader.uniformVars[ii];
				// CafeGLSL emits an aggregate record and per-element aliases. Bind the aggregate once.
				const char* bracket = strrchr(var.name, '[');
				if (bracket != NULL)
				{
					const std::string aggregate(var.name, bracket);
					bool alias = false;
					for (uint32_t jj = 0; jj < _shader.uniformVarCount; ++jj)
					{
						const GX2UniformVar& base = _shader.uniformVars[jj];
						if (aggregate == base.name && base.count > 1 && base.type == var.type && base.block == var.block)
						{
							const char* suffix = bracket + 1;
							while (*suffix >= '0' && *suffix <= '9') ++suffix;
							alias = suffix != bracket + 1 && suffix[0] == ']' && suffix[1] == '\0';
							break;
						}
					}
					if (alias) continue;
				}
				if (strchr(var.name, '.') != NULL)
				{
					bx::write(_messages, &err, "GX2: struct uniform %s cannot use a bgfx uniform handle; declare vec4/mat3/mat4 uniforms or arrays instead.\n", var.name);
					return false;
				}
				Uniform uniform;
				// Homogeneous multidimensional arrays flatten to the same bgfx handle.
				uniform.name.assign(var.name, strcspn(var.name, "["));
				uint32_t rows;
				switch (var.type)
				{
				case GX2_SHADER_VAR_TYPE_FLOAT:
				case GX2_SHADER_VAR_TYPE_FLOAT2:
				case GX2_SHADER_VAR_TYPE_FLOAT3:
				case GX2_SHADER_VAR_TYPE_FLOAT4: uniform.type = UniformType::Vec4; rows = 1; break;
				case GX2_SHADER_VAR_TYPE_FLOAT3X3: uniform.type = UniformType::Mat3; rows = 3; break;
				case GX2_SHADER_VAR_TYPE_FLOAT4X4: uniform.type = UniformType::Mat4; rows = 4; break;
				default:
					bx::write(_messages, &err, "GX2: unsupported bgfx uniform type for %s.\n", var.name);
					return false;
				}
				if (var.block < 0 || strcmp(_shader.uniformBlocks[var.block].name, "__cafe_loose_uniforms") != 0
				|| _shader.uniformBlocks[var.block].offset != 0 || var.count > UINT8_MAX || var.offset > UINT16_MAX
				|| rows * var.count > UINT16_MAX)
				{
					bx::write(_messages, &err, "GX2: %s is outside the loose-uniform block-zero ABI.\n", var.name);
					return false;
				}
				uniform.num = uint8_t(var.count);
				uniform.regIndex = uint16_t(var.offset); // Native reflection uses four-byte units.
				uniform.regCount = uint16_t(rows * var.count);
				auto aggregate = std::find_if(uniforms.begin(), uniforms.end(), [&](const Uniform& _other) { return _other.name == uniform.name; });
				if (aggregate != uniforms.end())
				{
					if (aggregate->type != uniform.type || aggregate->regIndex + aggregate->regCount * 4 != uniform.regIndex
					|| uint32_t(aggregate->num) + uniform.num > UINT8_MAX)
					{
						bx::write(_messages, &err, "GX2: multidimensional uniform %s cannot be flattened into a contiguous bgfx array.\n", var.name);
						return false;
					}
					aggregate->num = uint8_t(aggregate->num + uniform.num);
					aggregate->regCount += uniform.regCount;
				}
				else
				{
					uniforms.push_back(uniform);
				}
			}

			// GX2 reflection omits sampler counts and integer/compare/array texture shape.
			SamplerDeclarations declarations;
			if (!samplerDeclarations(_code, declarations, _messages)) return false;
			for (uint32_t ii = 0; ii < _shader.samplerVarCount; ++ii)
			{
				const GX2SamplerVar& sampler = _shader.samplerVars[ii];
				Uniform uniform;
				uniform.name = sampler.name;
				const auto declaration = declarations.find(uniform.name);
				if (declaration == declarations.end() || declaration->second.count == 0)
				{
					bx::write(_messages, &err, "GX2: sampler %s requires a plain declaration with a literal array size in 1..16 (macros may expand to literals).\n", sampler.name);
					return false;
				}
				const uint32_t count = declaration->second.count;
				if (sampler.location + count > 16 || !samplerMetadata(uniform, declaration->second.type))
				{
					bx::write(_messages, &err, "GX2: unsupported sampler type or binding range for %s (binding %u, count %u; bgfx GX2 exposes 16 slots).\n", sampler.name, sampler.location, count);
					return false;
				}
				uniform.num = uint8_t(count);
				uniform.regIndex = uint16_t(sampler.location);
				uniform.regCount = uint16_t(count);
				uniforms.push_back(uniform);
			}

			RawBindings().write(_writer, &err);
			bx::writeLE(_writer, uint16_t(uniforms.size()), &err);
			for (const Uniform& uniform : uniforms)
			{
				bx::write(_writer, uint8_t(uniform.name.size()), &err);
				bx::write(_writer, uniform.name.data(), int32_t(uniform.name.size()), &err);
				bx::write(_writer, uint8_t(uint8_t(uniform.type) | (_options.shaderType == 'f' ? kUniformFragmentBit : 0)), &err);
				bx::write(_writer, uniform.num, &err);
				bx::writeLE(_writer, uniform.regIndex, &err);
				bx::writeLE(_writer, uniform.regCount, &err);
				bx::write(_writer, uniform.texComponent, &err);
				bx::write(_writer, uniform.texDimension, &err);
				bx::writeLE(_writer, uniform.texFormat, &err);
			}
			bx::writeLE(_writer, uint32_t(_gfd.size()), &err);
			bx::write(_writer, _gfd.data(), int32_t(_gfd.size()), &err);
			return true;
		}
	}

	bool compileGx2Shader(const Options& _options, uint32_t, const std::string& _code,
		bx::WriterI* _writer, bx::WriterI* _messages)
	{
		// Only the compiler sees original comments; metadata is read from declarations.
		const std::string declarations = std::regex_replace(_code, std::regex(R"(/\*[\s\S]*?\*/|//[^\n\r]*)"), " ");
		if (_options.raw)
		{
			const std::regex directive(R"((?:^|\n)[ \t]*#[ \t]*([A-Za-z_][A-Za-z0-9_]*))");
			for (std::sregex_iterator it(declarations.begin(), declarations.end(), directive), end; it != end; ++it)
			{
				const std::string name = (*it)[1].str();
				if (name != "version" && name != "extension" && name != "line" && name != "pragma")
				{
					bx::ErrorAssert err;
					bx::write(_messages, &err, "GX2 --raw requires preprocessed GLSL; #%s is unsupported.\n", name.c_str());
					return false;
				}
			}
		}
		CompilerSession session;
		if (_options.keepIntermediate)
		{
			writeFile((_options.outputFilePath + ".glsl").c_str(), _code.data(), int32_t(_code.size()));
		}
		char diagnostics[16384] = {};
		const GLSL_COMPILER_FLAG flags = _options.disasm ? GLSL_COMPILER_FLAG_PRINT_DISASSEMBLY_TO_STDERR : GLSL_COMPILER_FLAG_NONE;
		GX2VertexShader* vertex = NULL;
		GX2PixelShader* pixel = NULL;
		const GX2ShaderMode mode = _options.shaderType == 'v'
			? CompileVertexShader(_code.c_str(), GLSL_COMPILE_BLOCK, &vertex, diagnostics, sizeof(diagnostics), flags)
			: CompilePixelShader(_code.c_str(), GLSL_COMPILE_BLOCK, &pixel, diagnostics, sizeof(diagnostics), flags);
		std::unique_ptr<GX2VertexShader, decltype(&FreeVertexShader)> vs(vertex, FreeVertexShader);
		std::unique_ptr<GX2PixelShader, decltype(&FreePixelShader)> fs(pixel, FreePixelShader);
		bx::ErrorAssert err;
		if (diagnostics[0] != '\0') bx::write(_messages, &err, "%s\n", diagnostics);
		if (mode == GLSL_SHADER_MODE_ERROR) return false;
		GFDFile file;
		if (vertex) file.vertexShaders.push_back(vertex);
		if (pixel) file.pixelShaders.push_back(pixel);
		const std::vector<uint8_t> bytes = SerializeGFD(file);
		return vertex ? writeStage(_options, *vertex, declarations, bytes, _writer, _messages)
			: writeStage(_options, *pixel, declarations, bytes, _writer, _messages);
	}
}
#else
namespace bgfx
{
	bool compileGx2Shader(const Options& _options, uint32_t _version, const std::string& _code,
		bx::WriterI* _writer, bx::WriterI* _messages)
	{
		BX_UNUSED(_options, _version, _code, _writer);
		bx::ErrorAssert err;
		bx::write(_messages, &err, "CafeGLSL compiler is not compiled in.\n");
		return false;
	}
}
#endif
