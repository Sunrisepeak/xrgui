module;

// `check` needs FT_Error_String, and the error strings only exist when
// FreeType was configured for them -- same preamble as font.ixx, because an
// implementation unit does NOT inherit its interface's includes or imports.
#if DEBUG_CHECK
#define FT_CONFIG_OPTION_ERROR_STRINGS
#endif

#include <cassert>
#include <ft2build.h>
#include <freetype/freetype.h>

module mo_yanxi.font;
import mo_yanxi.concurrent.guard;
import mo_yanxi.log;

namespace mo_yanxi::font{
U u;

// `check` used to be defined in font.ixx behind `module : private;`. That was
// ill-formed: a module unit carrying a private-module-fragment must be the ONLY
// module unit of its module ([module.unit]) -- and this file is a second one.
// IFNDR, so no compiler was obliged to say anything, and MSVC did not.
//
// An implementation unit is the mechanism that actually expresses the intent:
// the definition is invisible to importers, so changing it does not invalidate
// their BMIs. Same goal, correctly spelled -- and unlike the private fragment
// it is implemented everywhere (GCC 16 answers `module : private;` with
// "sorry, unimplemented").
void check(FT_Error error){
	if(!error) return;

#if DEBUG_CHECK
	const char* err = FT_Error_String(error);
	log::error({"Freetype"}, "error {}: {}", error, err);
#else
	log::error({"Freetype"}, "error {}", error);
#endif

	throw std::runtime_error("Freetype Failed");
}

float font_face_view::get_line_spacing(const math::usize2 sz) const{
	check(face().set_size(sz));
	return normalize_len(face()->size->metrics.height);
}

math::vec2 font_face_view::get_line_spacing_vec(const math::usize2 sz) const{
	check(face().set_size(sz));
	return get_line_spacing_vec();
}

math::vec2 font_face_view::get_line_spacing_vec() const{
	//TODO col distance is not good here.
	return {normalize_len(face()->size->metrics.max_advance), normalize_len(face()->size->metrics.height)};

}

math::usize2 font_face_view::get_font_pixel_spacing(const math::usize2 sz) const{
	check(face().set_size(sz));
	return {face()->size->metrics.x_ppem, face()->size->metrics.y_ppem};
}
}
