#include <emulator/emulator.hpp>
#include <sfc/interface/interface.hpp>
#include <filter/filter.hpp>
#include <lzma/lzma.hpp>
#include <nall/directory.hpp>
#include <nall/instance.hpp>
#include <nall/decode/rle.hpp>
#include <nall/decode/zip.hpp>
#include <nall/encode/rle.hpp>
#include <nall/encode/zip.hpp>
#include <nall/hash/crc16.hpp>
using namespace nall;

#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>
#include <heuristics/bs-memory.cpp>

#include "resources.hpp"

#include "libretro.h"
#define RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0)

static Emulator::Interface *emulator;

static bool sgb_border_disabled = false;

// Touchscreen Lightgun Support
static const int POINTER_PRESSED_CYCLES = 4;  // For touchscreen sensitivity
struct retro_pointer_state
{
	int x;
	int y;
	bool superscope_trigger_pressed;
	bool superscope_cursor_pressed;
	bool superscope_turbo_pressed;
	bool superscope_start_pressed;
	
	int pointer_pressed = 0;
	int pointer_cycles_after_released = 0;
	int pointer_pressed_last_x = 0;
	int pointer_pressed_last_y = 0;
};

static retro_pointer_state retro_pointer = { 0, 0, false, false, false, false };
static bool retro_pointer_enabled = false;
static bool retro_pointer_superscope_reverse_buttons = false;
static void input_update_pointer_lightgun( unsigned port, unsigned gun_device)
{
	int x, y;
	x = input_state(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
	y = input_state(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);

	int screen_width = 256;
	int screen_height = 224;

	/*scale & clamp*/
	x = ( ( x + 0x7FFF ) * screen_width ) / 0xFFFF;
	if ( x < 0 )
		x = 0;
	else if ( x >= screen_width )
		x = screen_width - 1;

	/*scale & clamp*/
	y = ( ( y + 0x7FFF ) * screen_height ) / 0xFFFF;
	if ( y < 0 )
		y = 0;
	else if ( y >= screen_height )
		y = screen_height - 1;

	// Touch sensitivity: Keep the gun position held for a fixed number of cycles after touch is released
	// because a very light touch can result in a misfire
	if ( retro_pointer.pointer_cycles_after_released > 0 && retro_pointer.pointer_cycles_after_released < POINTER_PRESSED_CYCLES ) {
			retro_pointer.pointer_cycles_after_released++;
			retro_pointer.x = retro_pointer.pointer_pressed_last_x;
			retro_pointer.y = retro_pointer.pointer_pressed_last_y;
			return;
	}

	if ( input_state( port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED ) )
	{
			retro_pointer.pointer_pressed = 1;
			retro_pointer.pointer_cycles_after_released = 0;
			retro_pointer.pointer_pressed_last_x = x;
			retro_pointer.pointer_pressed_last_y = y;
	} else if ( retro_pointer.pointer_pressed ) {
			retro_pointer.pointer_cycles_after_released++;
			retro_pointer.pointer_pressed = 0;
			x = retro_pointer.pointer_pressed_last_x;
			y = retro_pointer.pointer_pressed_last_y;
			// unpress the primary trigger
			if (retro_pointer_superscope_reverse_buttons)
				retro_pointer.superscope_cursor_pressed = false;
			else
				retro_pointer.superscope_trigger_pressed = false;
			return;
    }
		retro_pointer.x = x;
		retro_pointer.y = y;

    // triggers
    switch (gun_device)
    {
        case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
        {
					bool start_pressed = false;
					bool trigger_pressed = false;
					bool turbo_pressed = false;
					bool cursor_pressed = false;
					if ( input_state(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) ) {
							int touch_count = input_state(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);
							if ( touch_count == 4 ) {
									// start button
									start_pressed = true;
							} else if ( touch_count == 3 ) {
									turbo_pressed = true;
							} else if ( touch_count == 2 ) {
								if ( retro_pointer_superscope_reverse_buttons )
								{
									trigger_pressed = true;
								} else
								{
									cursor_pressed = true;
								}
							} else {
								if ( retro_pointer_superscope_reverse_buttons )
								{
									cursor_pressed = true;
								} else
								{
									trigger_pressed = true;
								}
							}
					}
					retro_pointer.superscope_trigger_pressed = trigger_pressed;
					retro_pointer.superscope_cursor_pressed = cursor_pressed;
					retro_pointer.superscope_start_pressed = start_pressed;
					retro_pointer.superscope_turbo_pressed = turbo_pressed;
					break;
				}
				default:
					break;
    }
}

static int input_handle_touchscreen_lightgun( unsigned port, unsigned gun_device, unsigned inputId)
{
	input_update_pointer_lightgun(port, gun_device);
	switch (inputId)
	{
		case 0: // X
			return retro_pointer.x;
		case 1: // Y
			return retro_pointer.y;
		case 2: // Trigger
			return retro_pointer.superscope_trigger_pressed ? 1 : 0;
		case 3: // Cursor
			return retro_pointer.superscope_cursor_pressed ? 1 : 0;
		case 4: // Turbo
			return retro_pointer.superscope_turbo_pressed ? 1 : 0;
		case 5: // Pause
			return retro_pointer.superscope_start_pressed ? 1 : 0;
		default:
			return 0; // Unknown input
	} 
}

struct Program : Emulator::Platform
{
	Program();
	~Program();
	
	auto open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file> override;
	auto load(uint id, string name, string type, vector<string> options = {}) -> Emulator::Platform::Load override;
	auto videoFrame(const uint16* data, uint pitch, uint width, uint height, uint scale) -> void override;
	auto audioFrame(const double* samples, uint channels) -> void override;
	auto inputPoll(uint port, uint device, uint input) -> int16 override;
	auto inputRumble(uint port, uint device, uint input, bool enable) -> void override;
	
	auto load() -> void;
	auto loadFile(string location) -> vector<uint8_t>;
	auto loadSuperFamicom(string location) -> bool;
	auto loadGameBoy(string location) -> bool;
	auto loadBSMemory(string location) -> bool;

	auto save() -> void;

	auto openRomSuperFamicom(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
	auto openRomGameBoy(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
	auto openRomBSMemory(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
	
	auto hackPatchMemory(vector<uint8_t>& data) -> void;
	
	string base_name;

	bool overscan = false;

public:	
	struct Game {
		explicit operator bool() const { return (bool)location; }
		
		string option;
		string location;
		string manifest;
		Markup::Node document;
		boolean patched;
		boolean verified;
	};

	struct SuperFamicom : Game {
		string title;
		string region;
		vector<uint8_t> program;
		vector<uint8_t> data;
		vector<uint8_t> expansion;
		vector<uint8_t> firmware;
	} superFamicom;

	struct GameBoy : Game {
		vector<uint8_t> program;
	} gameBoy;

	struct BSMemory : Game {
		vector<uint8_t> program;
	} bsMemory;
};

static Program *program = nullptr;

Program::Program()
{
	Emulator::platform = this;
}

Program::~Program()
{
	delete emulator;
}

auto Program::save() -> void
{
	if(!emulator->loaded()) return;
	emulator->save();
}

auto Program::open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file>
{
	shared_pointer<vfs::file> result;

	if (name == "ipl.rom" && mode == vfs::file::mode::read) {
		result = vfs::memory::file::open(iplrom, sizeof(iplrom));
	}

	if (name == "boards.bml" && mode == vfs::file::mode::read) {
		result = vfs::memory::file::open(Boards, sizeof(Boards));
	}

	if (id == 1) { //Super Famicom
		if (name == "manifest.bml" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(superFamicom.manifest.data<uint8_t>(), superFamicom.manifest.size());
		}
		else if (name == "program.rom" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(superFamicom.program.data(), superFamicom.program.size());
		}
		else if (name == "data.rom" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(superFamicom.data.data(), superFamicom.data.size());
		}
		else if (name == "expansion.rom" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(superFamicom.expansion.data(), superFamicom.expansion.size());
		}
		else {
			result = openRomSuperFamicom(name, mode);
		}
	}
	else if (id == 2) { //Game Boy
		if (name == "manifest.bml" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(gameBoy.manifest.data<uint8_t>(), gameBoy.manifest.size());
		}
		else if (name == "program.rom" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(gameBoy.program.data(), gameBoy.program.size());
		}
		else {
			result = openRomGameBoy(name, mode);
		}
	}
	else if (id == 3) {  //BS Memory
		if (name == "manifest.bml" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(bsMemory.manifest.data<uint8_t>(), bsMemory.manifest.size());
		}
		else if (name == "program.rom" && mode == vfs::file::mode::read) {
			result = vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
		}
		else if(name == "program.flash") {
			//writes are not flushed to disk in bsnes
			result = vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
		}
		else {
			result = openRomBSMemory(name, mode);
		}
	}
	return result;
}

auto Program::load() -> void {
	emulator->unload();
	emulator->load();

	// per-game hack overrides
	auto title = superFamicom.title;
	auto region = superFamicom.region;

	//relies on mid-scanline rendering techniques
	if(title == "AIR STRIKE PATROL" || title == "DESERT FIGHTER") emulator->configure("Hacks/PPU/Fast", false);

	//the dialogue text is blurry due to an issue in the scanline-based renderer's color math support
	if(title == "マーヴェラス") emulator->configure("Hacks/PPU/Fast", false);

	//stage 2 uses pseudo-hires in a way that's not compatible with the scanline-based renderer
	if(title == "SFC クレヨンシンチャン") emulator->configure("Hacks/PPU/Fast", false);

	//title screen game select (after choosing a game) changes OAM tiledata address mid-frame
	//this is only supported by the cycle-based PPU renderer
	if(title == "Winter olympics") emulator->configure("Hacks/PPU/Fast", false);

	//title screen shows remnants of the flag after choosing a language with the scanline-based renderer
	if(title == "WORLD CUP STRIKER") emulator->configure("Hacks/PPU/Fast", false);

	//relies on cycle-accurate writes to the echo buffer
	if(title == "KOUSHIEN_2") emulator->configure("Hacks/DSP/Fast", false);

	//will hang immediately
	if(title == "RENDERING RANGER R2") emulator->configure("Hacks/DSP/Fast", false);

	//will hang sometimes in the "Bach in Time" stage
	if(title == "BUBSY II" && region == "PAL") emulator->configure("Hacks/DSP/Fast", false);

	//fixes an errant scanline on the title screen due to writing to PPU registers too late
	if(title == "ADVENTURES OF FRANKEN" && region == "PAL") emulator->configure("Hacks/PPU/RenderCycle", 32);

	//fixes an errant scanline on the title screen due to writing to PPU registers too late
	if(title == "FIREPOWER 2000" || title == "SUPER SWIV") emulator->configure("Hacks/PPU/RenderCycle", 32);

	//fixes an errant scanline on the title screen due to writing to PPU registers too late
	if(title == "NHL '94" || title == "NHL PROHOCKEY'94") emulator->configure("Hacks/PPU/RenderCycle", 32);

	//fixes an errant scanline on the title screen due to writing to PPU registers too late
	if(title == "Sugoro Quest++") emulator->configure("Hacks/PPU/RenderCycle", 128);

	if (emulator->configuration("Hacks/Hotfixes")) {
		//this game transfers uninitialized memory into video RAM: this can cause a row of invalid tiles
		//to appear in the background of stage 12. this one is a bug in the original game, so only enable
		//it if the hotfixes option has been enabled.
		if(title == "The Hurricanes") emulator->configure("Hacks/Entropy", "None");

		//Frisky Tom attract sequence sometimes hangs when WRAM is initialized to pseudo-random patterns
		if (title == "ニチブツ・アーケード・クラシックス") emulator->configure("Hacks/Entropy", "None");
	}

	emulator->power();
}

auto Program::load(uint id, string name, string type, vector<string> options) -> Emulator::Platform::Load {
	if (id == 1)
	{
		if (loadSuperFamicom(superFamicom.location))
		{
			return {id, superFamicom.region};
		}
	}
	else if (id == 2)
	{
		if (loadGameBoy(gameBoy.location))
		{
			return { id, NULL };
		}
	}
	else if (id == 3) {
		if (loadBSMemory(bsMemory.location)) {
			return { id, NULL };
		}
	}
	return { id, options(0) };
}

auto Program::videoFrame(const uint16* data, uint pitch, uint width, uint height, uint scale) -> void {
	if (!overscan)
	{
		uint multiplier = height / 240;
		if ((sgb_border_disabled) && (program->gameBoy.program))
		{
			data += 47 * (pitch >> 1) * multiplier;
		}
		else
		{
			data += 8 * (pitch >> 1) * multiplier;
		}
		if (program->gameBoy.program)
		{
			height -= 16.1 * multiplier;
		}
		else
		{
			height -= 16 * multiplier;
		}
	}
	if ((!overscan) & (sgb_border_disabled) && (program->gameBoy.program))
	{
		video_cb(data + 48, width - 96, height - 79, pitch);
	}
	else
	{
		video_cb(data, width, height, pitch);
	}
}

// Double the fun!
static int16_t d2i16(double v)
{
	v *= 0x8000;
	if (v > 0x7fff)
		v = 0x7fff;
	else if (v < -0x8000)
		v = -0x8000;
	return int16_t(floor(v + 0.5));
}

auto Program::audioFrame(const double* samples, uint channels) -> void
{
	int16_t left = d2i16(samples[0]);
	int16_t right = d2i16(samples[1]);
	//audio_cb(left, right);
	audio_queue(left, right);
}

auto pollInputDevices(uint port, uint device, uint input) -> int16
{
	// TODO: This will need to be remapped on a per-system basis.
	unsigned libretro_port;
	unsigned libretro_id;
	unsigned libretro_device;
	unsigned libretro_index = 0;

	static const unsigned joypad_mapping[] = {
		RETRO_DEVICE_ID_JOYPAD_UP,
		RETRO_DEVICE_ID_JOYPAD_DOWN,
		RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_RIGHT,
		RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_A,
		RETRO_DEVICE_ID_JOYPAD_Y,
		RETRO_DEVICE_ID_JOYPAD_X,
		RETRO_DEVICE_ID_JOYPAD_L,
		RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_SELECT,
		RETRO_DEVICE_ID_JOYPAD_START,
	};

	static const unsigned mouse_mapping[] = {
		RETRO_DEVICE_ID_MOUSE_X,
		RETRO_DEVICE_ID_MOUSE_Y,
		RETRO_DEVICE_ID_MOUSE_LEFT,
		RETRO_DEVICE_ID_MOUSE_RIGHT,
	};

	switch (port)
	{
		case SuperFamicom::ID::Port::Controller1:
			libretro_port = 0;
			break;
		case SuperFamicom::ID::Port::Controller2:
			libretro_port = 1;
			break;

		default:
			return 0;
	}

	switch (device)
	{
		case SuperFamicom::ID::Device::Gamepad:
			libretro_device = RETRO_DEVICE_JOYPAD;
			libretro_id = joypad_mapping[input];
			break;

		case SuperFamicom::ID::Device::Mouse:
			libretro_device = RETRO_DEVICE_MOUSE;
			libretro_id = mouse_mapping[input];
			break;

		case SuperFamicom::ID::Device::SuperMultitap:
			libretro_device = RETRO_DEVICE_JOYPAD; // Maps to player [2, 5].
			libretro_port += input / 12;
			libretro_id = joypad_mapping[input % 12];
			break;

		// TODO: SuperScope/Justifiers.
		// Do we care? The v94 port hasn't hooked them up. :)
		case SuperFamicom::ID::Device::SuperScope:
		{
			libretro_device = RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE;
			if (retro_pointer_enabled)
			{
				return input_handle_touchscreen_lightgun(libretro_port, libretro_device, input);
			} else {
				return 0;
			}
		}
		

		default:
			return 0;
	}

	return input_state(libretro_port, libretro_device, libretro_index, libretro_id);
}

auto Program::inputPoll(uint port, uint device, uint input) -> int16
{
	return pollInputDevices(port, device, input);
}

auto Program::inputRumble(uint port, uint device, uint input, bool enable) -> void
{
}

auto Program::openRomSuperFamicom(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>
{
	if(name == "program.rom" && mode == vfs::file::mode::read)
	{
		return vfs::memory::file::open(superFamicom.program.data(), superFamicom.program.size());
	}

	if(name == "data.rom" && mode == vfs::file::mode::read)
	{
		return vfs::memory::file::open(superFamicom.data.data(), superFamicom.data.size());
	}

	if(name == "expansion.rom" && mode == vfs::file::mode::read)
	{
		return vfs::memory::file::open(superFamicom.expansion.data(), superFamicom.expansion.size());
	}

	if(name == "msu1/data.rom")
	{
		return vfs::fs::file::open({Location::notsuffix(superFamicom.location), ".msu"}, mode);
	}

	if(name.match("msu1/track*.pcm"))
	{
		name.trimLeft("msu1/track", 1L);
		return vfs::fs::file::open({Location::notsuffix(superFamicom.location), name}, mode);
	}

	if(name == "save.ram")
	{
		string save_path;

		auto suffix = Location::suffix(base_name);
		auto base = Location::base(base_name.transform("\\", "/"));

		const char *save = nullptr;
		if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save)
			save_path = { string(save).transform("\\", "/"), "/", base.trimRight(suffix, 1L), ".srm" };
		else
			save_path = { base_name.trimRight(suffix, 1L), ".srm" };

		return vfs::fs::file::open(save_path, mode);
	}

	return {};
}

auto Program::openRomGameBoy(string name, vfs::file::mode mode) -> shared_pointer<vfs::file> {
	if(name == "program.rom" && mode == vfs::file::mode::read)
	{
		return vfs::memory::file::open(gameBoy.program.data(), gameBoy.program.size());
	}

	if(name == "save.ram")
	{
		string save_path;

		auto suffix = Location::suffix(base_name);
		auto base = Location::base(base_name.transform("\\", "/"));

		const char *save = nullptr;
		if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save)
			save_path = { string(save).transform("\\", "/"), "/", base.trimRight(suffix, 1L), ".srm" };
		else
			save_path = { base_name.trimRight(suffix, 1L), ".srm" };

		return vfs::fs::file::open(save_path, mode);
	}

	if(name == "time.rtc")
	{
		string save_path;

		auto suffix = Location::suffix(base_name);
		auto base = Location::base(base_name.transform("\\", "/"));

		const char *save = nullptr;
		if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save)
			save_path = { string(save).transform("\\", "/"), "/", base.trimRight(suffix, 1L), ".rtc" };
		else
			save_path = { base_name.trimRight(suffix, 1L), ".rtc" };

		return vfs::fs::file::open(save_path, mode);
	}

	return {};
}

auto Program::openRomBSMemory(string name, vfs::file::mode mode) -> shared_pointer<vfs::file> {
	if (name == "program.rom" && mode == vfs::file::mode::read)
	{
		return vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
	}

	if (name == "program.flash")
	{
		//writes are not flushed to disk
		return vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
	}

	return {};
}

auto Program::loadFile(string location) -> vector<uint8_t>
{
	if(Location::suffix(location).downcase() == ".zip") {
		Decode::ZIP archive;
		if(archive.open(location)) {
			for(auto& file : archive.file) {
				auto type = Location::suffix(file.name).downcase();
				if(type == ".sfc" || type == ".smc" || type == ".gb" || type == ".gbc" || type == ".bs" || type == ".st") {
					return archive.extract(file);
				}
			}
		}
	return {};
	}
	else if(Location::suffix(location).downcase() == ".7z") {
		return LZMA::extract(location);
	}
	else {
		return file::read(location);
	}
}

auto Program::loadSuperFamicom(string location) -> bool
{
	vector<uint8_t> rom;
	rom = loadFile(location);

	if(rom.size() < 0x8000) return false;

	//assume ROM and IPS agree on whether a copier header is present
	//superFamicom.patched = applyPatchIPS(rom, location);
	if((rom.size() & 0x7fff) == 512) {
		//remove copier header
		memory::move(&rom[0], &rom[512], rom.size() - 512);
		rom.resize(rom.size() - 512);
	}

	auto heuristics = Heuristics::SuperFamicom(rom, location);
	auto sha256 = Hash::SHA256(rom).digest();

	superFamicom.title = heuristics.title();
	superFamicom.region = heuristics.videoRegion();
	superFamicom.manifest = heuristics.manifest();

	hackPatchMemory(rom);
	superFamicom.document = BML::unserialize(superFamicom.manifest);
	superFamicom.location = location;

	uint offset = 0;
	if(auto size = heuristics.programRomSize()) {
		superFamicom.program.resize(size);
		memory::copy(&superFamicom.program[0], &rom[offset], size);
		offset += size;
	}
	if(auto size = heuristics.dataRomSize()) {
		superFamicom.data.resize(size);
		memory::copy(&superFamicom.data[0], &rom[offset], size);
		offset += size;
	}
	if(auto size = heuristics.expansionRomSize()) {
		superFamicom.expansion.resize(size);
		memory::copy(&superFamicom.expansion[0], &rom[offset], size);
		offset += size;
	}
	if(auto size = heuristics.firmwareRomSize()) {
		superFamicom.firmware.resize(size);
		memory::copy(&superFamicom.firmware[0], &rom[offset], size);
		offset += size;
	}
	return true;
}

auto Program::loadGameBoy(string location) -> bool {
	vector<uint8_t> rom;
	rom = loadFile(location);

	if (rom.size() < 0x4000) return false;

	auto heuristics = Heuristics::GameBoy(rom, location);
	auto sha256 = Hash::SHA256(rom).digest();

	gameBoy.manifest = heuristics.manifest();
	gameBoy.document = BML::unserialize(gameBoy.manifest);
	gameBoy.location = location;
	gameBoy.program = rom;

	return true;
}

auto Program::loadBSMemory(string location) -> bool {
	vector<uint8_t> rom;
	rom = loadFile(location);

	if (rom.size() < 0x8000) return false;

	auto heuristics = Heuristics::BSMemory(rom, location);
	auto sha256 = Hash::SHA256(rom).digest();

	bsMemory.manifest = heuristics.manifest();
	bsMemory.document = BML::unserialize(bsMemory.manifest);
	bsMemory.location = location;

	bsMemory.program = rom;
	return true;
}

auto Program::hackPatchMemory(vector<uint8_t>& data) -> void
{
	auto title = superFamicom.title;

	if(title == "Satellaview BS-X" && data.size() >= 0x100000) {
		//BS-X: Sore wa Namae o Nusumareta Machi no Monogatari (JPN) (1.1)
		//disable limited play check for BS Memory flash cartridges
		//benefit: allow locked out BS Memory flash games to play without manual header patching
		//detriment: BS Memory ROM cartridges will cause the game to hang in the load menu
		if(data[0x4a9b] == 0x10) data[0x4a9b] = 0x80;
		if(data[0x4d6d] == 0x10) data[0x4d6d] = 0x80;
		if(data[0x4ded] == 0x10) data[0x4ded] = 0x80;
		if(data[0x4e9a] == 0x10) data[0x4e9a] = 0x80;
	}
}

auto decodeSNES(string& code) -> bool {
  //Game Genie
  if(code.size() == 9 && code[4u] == '-') {
    //strip '-'
    code = {code.slice(0, 4), code.slice(5, 4)};
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //decode
    code.transform("df4709156bc8a23e", "0123456789abcdef");
    uint32_t r = toHex(code);
    //abcd efgh ijkl mnop qrst uvwx
    //ijkl qrst opab cduv wxef ghmn
    uint address =
      (!!(r & 0x002000) << 23) | (!!(r & 0x001000) << 22)
    | (!!(r & 0x000800) << 21) | (!!(r & 0x000400) << 20)
    | (!!(r & 0x000020) << 19) | (!!(r & 0x000010) << 18)
    | (!!(r & 0x000008) << 17) | (!!(r & 0x000004) << 16)
    | (!!(r & 0x800000) << 15) | (!!(r & 0x400000) << 14)
    | (!!(r & 0x200000) << 13) | (!!(r & 0x100000) << 12)
    | (!!(r & 0x000002) << 11) | (!!(r & 0x000001) << 10)
    | (!!(r & 0x008000) <<  9) | (!!(r & 0x004000) <<  8)
    | (!!(r & 0x080000) <<  7) | (!!(r & 0x040000) <<  6)
    | (!!(r & 0x020000) <<  5) | (!!(r & 0x010000) <<  4)
    | (!!(r & 0x000200) <<  3) | (!!(r & 0x000100) <<  2)
    | (!!(r & 0x000080) <<  1) | (!!(r & 0x000040) <<  0);
    uint data = r >> 24;
    code = {hex(address, 6L), "=", hex(data, 2L)};
    return true;
  }

  //Pro Action Replay
  if(code.size() == 8) {
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //decode
    uint32_t r = toHex(code);
    uint address = r >> 8;
    uint data = r & 0xff;
    code = {hex(address, 6L), "=", hex(data, 2L)};
    return true;
  }

  //higan: address=data
  if(code.size() == 9 && code[6u] == '=') {
    string nibbles = {code.slice(0, 6), code.slice(7, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //higan: address=compare?data
  if(code.size() == 12 && code[6u] == '=' && code[9u] == '?') {
    string nibbles = {code.slice(0, 6), code.slice(7, 2), code.slice(10, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //unrecognized code format
  return false;
}

auto decodeGB(string& code) -> bool {
  auto nibble = [&](const string& s, uint index) -> uint {
    if(index >= s.size()) return 0;
    if(s[index] >= '0' && s[index] <= '9') return s[index] - '0';
    return s[index] - 'a' + 10;
  };

  //Game Genie
  if(code.size() == 7 && code[3u] == '-') {
    code = {code.slice(0, 3), code.slice(4, 3)};
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    uint data = nibble(code, 0) << 4 | nibble(code, 1) << 0;
    uint address = (nibble(code, 5) ^ 15) << 12 | nibble(code, 2) << 8 | nibble(code, 3) << 4 | nibble(code, 4) << 0;
    code = {hex(address, 4L), "=", hex(data, 2L)};
    return true;
  }

  //Game Genie
  if(code.size() == 11 && code[3u] == '-' && code[7u] == '-') {
    code = {code.slice(0, 3), code.slice(4, 3), code.slice(8, 3)};
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    uint data = nibble(code, 0) << 4 | nibble(code, 1) << 0;
    uint address = (nibble(code, 5) ^ 15) << 12 | nibble(code, 2) << 8 | nibble(code, 3) << 4 | nibble(code, 4) << 0;
    uint8_t t = nibble(code, 6) << 4 | nibble(code, 8) << 0;
    t = t >> 2 | t << 6;
    uint compare = t ^ 0xba;
    code = {hex(address, 4L), "=", hex(compare, 2L), "?", hex(data, 2L)};
    return true;
  }

  //GameShark
  if(code.size() == 8) {
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //first two characters are the code type / VRAM bank, which is almost always 01.
    //other values are presumably supported, but I have no info on them, so they're not supported.
    if(code[0u] != '0') return false;
    if(code[1u] != '1') return false;
    uint data = toHex(code.slice(2, 2));
    uint16_t address = toHex(code.slice(4, 4));
    address = address >> 8 | address << 8;
    code = {hex(address, 4L), "=", hex(data, 2L)};
    return true;
  }

  //higan: address=data
  if(code.size() == 7 && code[4u] == '=') {
    string nibbles = {code.slice(0, 4), code.slice(5, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //higan: address=compare?data
  if(code.size() == 10 && code[4u] == '=' && code[7u] == '?') {
    string nibbles = {code.slice(0, 4), code.slice(5, 2), code.slice(8, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      if(n >= 'A' && n <= 'F') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //unrecognized code format
  return false;
}
