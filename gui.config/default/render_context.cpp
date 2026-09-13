module;

#include <vulkan/vulkan.h>
// Under mcpp the shaders are compiled into the binary by mcpp.rules.slang and
// reached through the surface it generates; the header lives in the build
// output and declares one accessor per shader. A conditional #include in the
// global module fragment is fine where a conditional `import` would not be:
// the module scanner reads imports, not includes.
#ifdef XRGUI_SHADER_SURFACE
#include <xrgui.shaders.h>
#endif

module mo_yanxi.gui.cfg.render_context;

import std;

import mo_yanxi.vk;

import mo_yanxi.graphic.g2d;
import mo_yanxi.gui.global;
import mo_yanxi.gui.assets.manager;
import mo_yanxi.gui.image_regions;
import mo_yanxi.gui.fx.instruction_extension;
import mo_yanxi.gui.cfg.builtin.assets;
import mo_yanxi.gui.cfg.builtin.font_styles;

import mo_yanxi.font;
import mo_yanxi.log;

namespace mo_yanxi::gui::cfg{
namespace{

std::filesystem::path default_shader_spv_path(){
	return std::filesystem::current_path().append("assets/shader/spv").make_preferred();
}

std::filesystem::path default_image_asset_path(){
	return std::filesystem::current_path().append("assets/images").make_preferred();
}

VkApplicationInfo make_default_application_info(const std::string& app_name){
	VkApplicationInfo app_info{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = app_name.c_str(),
		.applicationVersion = VK_MAKE_API_VERSION(1, 0, 0, 0),
		.pEngineName = "No Engine",
		.engineVersion = VK_MAKE_API_VERSION(1, 0, 0, 0),
		.apiVersion = VK_API_VERSION_1_3,
	};

	if(std::uint32_t supported_version = 0; vkEnumerateInstanceVersion(&supported_version) == VK_SUCCESS){
		app_info.apiVersion = supported_version >= VK_API_VERSION_1_3
			                      ? VK_API_VERSION_1_3
			                      : VK_API_VERSION_1_0;
	}

	return app_info;
}

std::vector<VkSamplerCreateInfo> make_default_sampler_create_infos(){
	return {vk::preset::ui_texture_sampler};
}

VkSamplerCreateInfo normalize_sampler_create_info(VkSamplerCreateInfo create_info){
	if(create_info.sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO){
		return create_info;
	}

	return vk::preset::ui_texture_sampler;
}

graphic::image_page_sampler_indices make_default_image_page_sampler_indices(
	graphic::sampler_descriptor_index sampler_index){
	return {
		{graphic::image_page_usage::regular, sampler_index},
		{graphic::image_page_usage::normal, sampler_index},
		{graphic::image_page_usage::msdf, sampler_index},
	};
}

void normalize_sampler_create_infos(std::vector<VkSamplerCreateInfo>& create_infos){
	if(create_infos.empty()){
		create_infos = make_default_sampler_create_infos();
	}

	for(auto& create_info : create_infos){
		create_info = normalize_sampler_create_info(create_info);
	}
}

std::vector<graphic::sampler_descriptor_index> register_sampler_vector(
	graphic::image_view_registry& registry,
	const vk::sampler_vector& sampler_vector){
	std::vector<graphic::sampler_descriptor_index> registered_indices{};
	registered_indices.reserve(sampler_vector.size());
	for(const auto sampler : sampler_vector){
		registered_indices.push_back(registry.register_sampler(sampler));
	}
	return registered_indices;
}

graphic::image_page_sampler_indices resolve_image_page_sampler_indices(
	graphic::image_page_sampler_indices configured_indices,
	std::span<const graphic::sampler_descriptor_index> registered_sampler_indices){
	if(configured_indices.empty()){
		if(registered_sampler_indices.empty()){
			return {};
		}
		return make_default_image_page_sampler_indices(registered_sampler_indices.front());
	}

	for(auto& sampler_index : configured_indices | std::views::values){
		if(sampler_index == graphic::auto_sampler_index){
			continue;
		}

		if(sampler_index >= registered_sampler_indices.size()){
			throw std::invalid_argument{
				"render_context image_page_sampler_indices references a missing sampler_create_infos entry"
			};
		}
		sampler_index = registered_sampler_indices[sampler_index];
	}

	return configured_indices;
}

const VkApplicationInfo& prepare_config_for_context(render_context_config& config){
	if(config.app_info.sType == VK_STRUCTURE_TYPE_APPLICATION_INFO){
		config.app_info.pApplicationName = config.app_info.pApplicationName != nullptr
			                                  ? config.app_info.pApplicationName
			                                  : config.app_name.c_str();
	} else{
		config.app_info = make_default_application_info(config.app_name);
	}

	normalize_sampler_create_infos(config.sampler_create_infos);
	return config.app_info;
}

}

vk::shader_module load_builtin_shader(
	backend::vulkan::context& ctx,
	const std::string_view name,
	const std::filesystem::path& shader_spv_path){
#ifdef XRGUI_SHADER_SURFACE
	// The arrays mcpp.rules.slang compiled in, under the names the .spv files
	// carry; `shader_spv_path` is what the other build reads them from.
	(void)shader_spv_path;
	namespace sh = xrgui::shaders;
	static constexpr std::pair<std::string_view, sh::payload (*)()> table[]{
		{"ui.draw.vert",                         sh::ui::draw::vert},
		{"ui.draw.frag_basic",                   sh::ui::draw::frag_basic},
		{"ui.draw.frag_outlined",                sh::ui::draw::frag_outlined},
		{"ui.draw.coord_draw",                   sh::ui::draw::coord_draw},
		{"ui.draw.frag_mask",                    sh::ui::draw::frag_mask},
		{"ui.draw.frag_mask_apply",              sh::ui::draw::frag_mask_apply},
		{"ui.blit.basic",                        sh::ui::blit::lane_merge},   // config.toml's alias
		{"ui.blit.alpha_blend",                  sh::ui::blit::alpha_blend},
		{"ui.blit.inverse",                      sh::ui::blit::inverse},
		{"ui.instruction_resolve_comp",          sh::ui::instruction_resolve_comp},
		{"ui.merge",                             sh::ui::merge},
		{"post_process.highlight_extract",       sh::post_process::highlight_extract},
		{"post_process.fullscreen_present.vert", sh::post_process::fullscreen_present_vert},
		{"post_process.fullscreen_present.frag", sh::post_process::fullscreen_present_frag},
		{"post_process.bloom",                   sh::post_process::bloom},
	};
	for(const auto& [known, get] : table){
		if(known != name) continue;
		const auto p = get();
		return vk::shader_module{ctx.get_device(),
			std::span<const std::uint32_t>{p.code, p.size_bytes / sizeof(std::uint32_t)}};
	}
	throw std::invalid_argument{std::format("xrgui has no shader named '{}'", name)};
#else
	return vk::shader_module{ctx.get_device(), shader_spv_path / std::format("{}.spv", name)};
#endif
}

[[nodiscard]] renderer_create_info_bundle make_default_renderer_create_info(
	backend::vulkan::context& ctx,
	graphic::image_view_registry& image_view_registry,
	const std::filesystem::path& shader_spv_path){
	renderer_create_info_bundle bundle{};
	auto& shader_modules = bundle.shader_modules;
	shader_modules.reserve(10);

	auto load = [&](const std::string_view name) -> vk::shader_module& {
		return shader_modules.emplace_back(load_builtin_shader(ctx, name, shader_spv_path));
	};
	auto& draw_shader_vert = load("ui.draw.vert");
	auto& draw_shader_frag_basic = load("ui.draw.frag_basic");
	auto& draw_shader_frag_outlined = load("ui.draw.frag_outlined");
	auto& draw_shader_coord = load("ui.draw.coord_draw");
	auto& draw_shader_mask = load("ui.draw.frag_mask");
	auto& draw_shader_mask_apply = load("ui.draw.frag_mask_apply");

	auto& blit_shader_merge = load("ui.blit.basic");
	auto& blit_shader_blend = load("ui.blit.alpha_blend");
	auto& blit_shader_inverse = load("ui.blit.inverse");

	auto& shader_instr_resolve = load("ui.instruction_resolve_comp");

	using namespace backend::vulkan;
	bundle.create_info = renderer_create_info{
		.allocator_usage = ctx.get_allocator(),
		.command_queue_family = ctx.graphic_family(),
		.image_view_registry = &image_view_registry,
		.attachment_draw_config = {
			{
				draw_attachment_config{
					.attachment = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
				},
				draw_attachment_config{
					.attachment = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
				},
			},
		},
		.attachment_blit_config = {
			{
				attachment_config{VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
				attachment_config{VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
				attachment_config{VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
				attachment_config{VK_FORMAT_R16_UINT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
			}
		},
		.draw_pipe_config = graphic_pipeline_create_config{
			{
				graphic_pipeline_create_config::config{
					{
						draw_shader_vert.get_stage_bundle(VK_SHADER_STAGE_VERTEX_BIT, "main_vert"),
						draw_shader_frag_basic.get_stage_bundle(VK_SHADER_STAGE_FRAGMENT_BIT, "main_frag")
					},
					graphic_pipeline_option{
						false, mask_usage::ignore, {0b1}, {},
						{
							{vk::blending::premultiplied_alpha_blend}, blend_dynamic_flags::equation | blend_dynamic_flags::write_flag
						}
					}
				},
				graphic_pipeline_create_config::config{
					{
						draw_shader_vert.get_stage_bundle(VK_SHADER_STAGE_VERTEX_BIT, "main_vert"),
						draw_shader_frag_outlined.get_stage_bundle(VK_SHADER_STAGE_FRAGMENT_BIT, "main_frag")
					},
					graphic_pipeline_option{
						false, mask_usage::ignore, {0b1}, {},
						{
							{vk::blending::premultiplied_alpha_blend}
						}
					}
				},
				graphic_pipeline_create_config::config{
					{
						draw_shader_coord.get_stage_bundle(VK_SHADER_STAGE_VERTEX_BIT, "main_vert"),
						draw_shader_coord.get_stage_bundle(VK_SHADER_STAGE_FRAGMENT_BIT, "main_frag")
					},
					graphic_pipeline_option{
						false, mask_usage::ignore, {0b1}, {},
						{
							{vk::blending::premultiplied_alpha_blend}
						}
					}
				},
				graphic_pipeline_create_config::config{
					{
						draw_shader_vert.get_stage_bundle(VK_SHADER_STAGE_VERTEX_BIT, "main_vert"),
						draw_shader_mask.get_stage_bundle(VK_SHADER_STAGE_FRAGMENT_BIT, "main_frag")
					},
					graphic_pipeline_option{
						false, mask_usage::write, {}, {},
						{
							{vk::blending::mask_draw}, blend_dynamic_flags::equation
						}
					}
				},
				graphic_pipeline_create_config::config{
					{
						draw_shader_vert.get_stage_bundle(VK_SHADER_STAGE_VERTEX_BIT, "main_vert"),
						draw_shader_mask_apply.get_stage_bundle(VK_SHADER_STAGE_FRAGMENT_BIT, "main_frag")
					},
					graphic_pipeline_option{
						false, mask_usage::read, {0b1}, {},
						{
							{vk::blending::max_alpha_blend}
						}
					}
				},
			},
			{}
		},
		.blit_pipe_config = compute_pipeline_create_config{
			{
				compute_pipeline_create_config::config{
					.shader_bundle = blit_shader_merge.get_stage_bundle(VK_SHADER_STAGE_COMPUTE_BIT),
					.option = {
						.inout = compute_pipeline_blit_inout_config{
							{
								{0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
								{1, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
							},
							{
								{2, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
								{3, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
								{4, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
							}
						},
					}
				},
				compute_pipeline_create_config::config{
					.shader_bundle = blit_shader_blend.get_stage_bundle(VK_SHADER_STAGE_COMPUTE_BIT),
					.option = {
						.inout = compute_pipeline_blit_inout_config{
							{
								{0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
							},
							{
								{1, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
							}
						},
					}
				},
				compute_pipeline_create_config::config{
					.shader_bundle = blit_shader_inverse.get_stage_bundle(VK_SHADER_STAGE_COMPUTE_BIT),
					.option = {
						.inout = compute_pipeline_blit_inout_config{
							{
								{0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
							},
							{
								{1, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
							}
						},
					}
				},
			},
			{
				compute_pipeline_blit_inout_config{
					{
						{0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
					},
					{
						{1, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
					}
				}
			}
		},
		.resolver_shader_stage = shader_instr_resolve.get_create_info(VK_SHADER_STAGE_COMPUTE_BIT),
		.stride_config = {
			.vertex_stride = sizeof(gui_vertex_mock),
			.primitive_stride = sizeof(gui_primitive_mock),
		}
	};
	return bundle;
}

render_context::render_context(render_context_config config)
	: config_(std::move(config)),
	  ctx_(prepare_config_for_context(config_)),
	  rich_text_table_{
		  ([this]{
			  vk::load_ext(ctx_.get_instance());
			  vk::register_default_requirements(ctx_.get_device(), ctx_.get_physical_device());
		  }(), typesetting::rich_text_look_up_table{})
	  },
	  sampler_vector_(ctx_.get_device(), config_.sampler_create_infos),
	  atlas_({
		  .ctx_info = ctx_,
		  .graphic_family_index = ctx_.graphic_family(),
		  .loader_working_queue = ctx_.get_device().graphic_queue(1),
		  .image_view_registry = std::addressof(image_view_registry_),
		  .page_sampler_indices = [this]{
			  auto registered_sampler_indices = register_sampler_vector(image_view_registry_, sampler_vector_);
			  return resolve_image_page_sampler_indices(
				  std::move(config_.image_page_sampler_indices),
				  registered_sampler_indices);
		  }()
	  }){
	try{
		initialize();
	} catch(...){
		shutdown();
		throw;
	}
}

render_context::~render_context(){
	shutdown();
}

void render_context::initialize(){
	log::info({"Lifecycle"}, "render_context initialization started");

	log::debug({"Lifecycle"}, "Setting rich_text lookup table");
	typesetting::look_up_table = &rich_text_table_;

	if(config_.initialize_gui_globals){
		log::debug({"Lifecycle"}, "Initializing GUI globals");
		gui::global::initialize();
		gui_initialized_ = true;

		//TODO shuold this be set in render context??
		ctx_.window().set_input_sink(&gui::global::event_queue);

		log::debug({"Lifecycle"}, "Initializing assets manager");
		gui::global::initialize_assets_manager(gui::global::manager.get_arena_id());
		assets_manager_initialized_ = true;
	}

	log::debug({"Lifecycle"}, "Initializing font manager");
	builtin::init_font_manager(fonts_, atlas_);

	if(config_.load_default_assets){
		load_default_assets();
	}

	log::info({"Lifecycle"}, "render_context initialization completed");
}

void render_context::load_default_assets(){
	log::debug({"Lifecycle"}, "Loading default assets");

	auto& logo_page = atlas_.create_image_page("tex.logo", {
		.extent = {1920, 1080},
		.format = VK_FORMAT_R8G8B8A8_SRGB,
		.margin = 0,
		.usage = graphic::image_page_usage::regular
	});

	const auto image_path = config_.image_asset_path.empty()
		                        ? default_image_asset_path()
		                        : config_.image_asset_path;
	auto rst = logo_page.register_named_region("logo", graphic::image_load_description{
		graphic::bitmap_path_load{(image_path / "logo.png").string()}
	}, true);

	gui::assets::builtin::get_page().insert(
		gui::assets::builtin::shape_id::logo,
		gui::constant_image_region_borrow{rst.region});

	builtin::generate_default_shapes(atlas_);
	builtin::load_default_icons(atlas_);
	generated_shapes_initialized_ = true;

	log::debug({"Lifecycle"}, "Default assets loaded");
}

backend::vulkan::context& render_context::context(){
	return ctx_;
}

vk::shader_module render_context::load_shader(const std::string_view name){
	return load_builtin_shader(context(), name,
		config_.shader_spv_path.empty() ? default_shader_spv_path() : config_.shader_spv_path);
}

renderer_create_info_bundle render_context::make_renderer_create_info(){
	auto& ctx = context();
	auto& registry = image_view_registry();
	const auto shader_path = config_.shader_spv_path.empty()
		                         ? default_shader_spv_path()
		                         : config_.shader_spv_path;
	auto bundle = make_default_renderer_create_info(ctx, registry, shader_path);
	if(config_.configure_renderer_create_info){
		config_.configure_renderer_create_info(bundle.create_info);
	}
	if(bundle.create_info.image_view_registry == nullptr){
		throw std::invalid_argument{"renderer_create_info requires a non-null image view registry"};
	}
	if(bundle.create_info.image_view_registry != std::addressof(registry)){
		throw std::invalid_argument{
			"render_context renderer_create_info must use the render_context image view registry"
		};
	}
	return bundle;
}

graphic::image_view_registry& render_context::image_view_registry(){
	return image_view_registry_;
}

const graphic::image_view_registry& render_context::image_view_registry() const{
	return image_view_registry_;
}

graphic::image_atlas& render_context::image_atlas(){
	return atlas_;
}

font::font_manager& render_context::font_manager(){
	return fonts_;
}

void render_context::wait_on_device(){
	context().wait_on_device();
}

void render_context::shutdown() noexcept{
	if(shutdown_done_){
		return;
	}
	shutdown_done_ = true;

	log::info({"Lifecycle"}, "render_context shutdown started");

	if(generated_shapes_initialized_){
		log::debug({"Lifecycle"}, "Disposing generated shapes");
		builtin::dispose_generated_shapes();
		generated_shapes_initialized_ = false;
	}

	log::debug({"Lifecycle"}, "Stopping image atlas async operations");
	atlas_.request_stop();

	log::debug({"Lifecycle"}, "Waiting on Vulkan device");
	try{
		ctx_.wait_on_device();
	} catch(...){
		log::error({"Lifecycle"}, "Exception during wait_on_device in render_context shutdown");
	}

	if(assets_manager_initialized_){
		log::debug({"Lifecycle"}, "Terminating assets manager");
		// The global assets manager stores borrowed image regions owned by atlas_.
		global::terminate_assets_manager();
		assets_manager_initialized_ = false;
	}

	log::debug({"Lifecycle"}, "Clearing font and typesetting globals");
	font::default_font_manager = nullptr;
	typesetting::look_up_table = nullptr;

	if(gui_initialized_){
		log::debug({"Lifecycle"}, "Terminating GUI globals");
		ctx_.window().set_input_sink(nullptr);
		global::terminate();
		gui_initialized_ = false;
	}

	log::info({"Lifecycle"}, "render_context shutdown completed");
}

}
