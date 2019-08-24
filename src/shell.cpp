/*
 * uuid-console - Microcontroller console shell
 * Copyright 2019  Simon Arlott
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <uuid/console.h>

#include <Arduino.h>
#include <stdarg.h>

#include <memory>
#include <list>
#include <set>
#include <string>
#include <vector>

#include <uuid/log.h>

#ifndef __cpp_lib_make_unique
namespace std {

template<typename _Tp, typename... _Args>
inline unique_ptr<_Tp> make_unique(_Args&&... __args) {
	return unique_ptr<_Tp>(new _Tp(std::forward<_Args>(__args)...));
}

} // namespace std
#endif

namespace uuid {

namespace console {

static const char __pstr__logger_name[] __attribute__((__aligned__(sizeof(int)))) PROGMEM = "shell";
const uuid::log::Logger Shell::logger_{FPSTR(__pstr__logger_name), uuid::log::Facility::LPR};
std::set<std::shared_ptr<Shell>> Shell::shells_;

Shell::Shell(std::shared_ptr<Commands> commands, unsigned int context, unsigned int flags)
		: commands_(commands), flags_(flags) {
	enter_context(context);
}

Shell::~Shell() {
	uuid::log::Logger::unregister_handler(this);
}

void Shell::start() {
	uuid::log::Logger::register_handler(this, uuid::log::Level::NOTICE);
	line_buffer_.reserve(maximum_command_line_length());
	display_banner();
	display_prompt();
	shells_.insert(shared_from_this());
	started();
};

bool Shell::running() const {
	return !stopped_;
}

void Shell::stop() {
	if (running()) {
		stopped_ = true;
		stopped();
	}
}

Shell::QueuedLogMessage::QueuedLogMessage(unsigned long id, std::shared_ptr<uuid::log::Message> content)
		: id_(id), content_(content) {

}

void Shell::operator<<(std::shared_ptr<uuid::log::Message> message) {
	if (log_messages_.size() >= maximum_log_messages()) {
		log_messages_.pop_front();
	}

	log_messages_.emplace_back(log_message_id_++, message);
}

uuid::log::Level Shell::get_log_level() const {
	return uuid::log::Logger::get_log_level(this);
}

void Shell::set_log_level(uuid::log::Level level) {
	uuid::log::Logger::register_handler(this, level);
}

void Shell::loop_all() {
	for (auto shell = shells_.begin(); shell != shells_.end(); ) {
		shell->get()->loop_one();

		// This avoids copying the shared_ptr every time loop_one() is called
		if (!shell->get()->running()) {
			shell = shells_.erase(shell);
		} else {
			shell++;
		}
	}
}

void Shell::loop_one() {
	output_logs();

	switch (mode_) {
	case Mode::NORMAL:
		loop_normal();
		break;

	case Mode::PASSWORD:
		loop_password();
		break;

	case Mode::DELAY:
		loop_delay();
		break;
	}
}

void Shell::loop_normal() {
	const int input = read_one_char();

	if (input < 0) {
		return;
	}

	const unsigned char c = input;

	switch (c) {
	case '\x03':
		// Interrupt (^C)
		line_buffer_.clear();
		println();
		prompt_displayed_ = false;
		display_prompt();
		break;

	case '\x04':
		// End of transmission (^D)
		if (line_buffer_.empty()) {
			end_of_transmission();
		}
		break;

	case '\x08':
	case '\x7F':
		// Backspace (^H)
		// Delete (^?)
		if (!line_buffer_.empty()) {
			erase_characters(1);
			line_buffer_.pop_back();
		}
		break;

	case '\x09':
		// Tab (^I)
		process_completion();
		break;

	case '\x0A':
		// Line feed (^J)
		if (previous_ != '\x0D') {
			process_command();
		}
		break;

	case '\x0C':
		// New page (^L)
		erase_current_line();
		display_prompt();
		break;

	case '\x0D':
		// Carriage return (^M)
		process_command();
		break;

	case '\x15':
		// Delete line (^U)
		erase_current_line();
		line_buffer_.clear();
		display_prompt();
		break;

	case '\x17':
		// Delete word (^W)
		delete_buffer_word(true);
		break;

	default:
		if (c >= '\x20' && c <= '\x7E') {
			// ASCII text
			if (line_buffer_.length() < maximum_command_line_length()) {
				line_buffer_.push_back(c);
				write((uint8_t)c);
			}
		}
		break;
	}

	previous_ = c;
}

Shell::PasswordData::PasswordData(const __FlashStringHelper *password_prompt, password_function password_function)
		: password_prompt_(password_prompt), password_function_(password_function) {

}

void Shell::loop_password() {
	const int input = read_one_char();

	if (input < 0) {
		return;
	}

	const unsigned char c = input;

	switch (c) {
	case '\x03':
		// Interrupt (^C)
		process_password(false);
		break;

	case '\x08':
	case '\x7F':
		// Backspace (^H)
		// Delete (^?)
		if (!line_buffer_.empty()) {
			line_buffer_.pop_back();
		}
		break;

	case '\x0A':
		// Line feed (^J)
		if (previous_ != '\x0D') {
			process_password(true);
		}
		break;

	case '\x0C':
		// New page (^L)
		erase_current_line();
		display_prompt();
		break;

	case '\x0D':
		// Carriage return (^M)
		process_password(true);
		break;

	case '\x15':
		// Delete line (^U)
		line_buffer_.clear();
		break;

	case '\x17':
		// Delete word (^W)
		delete_buffer_word(false);
		break;

	default:
		if (c >= '\x20' && c <= '\x7E') {
			// ASCII text
			if (line_buffer_.length() < maximum_command_line_length()) {
				line_buffer_.push_back(c);
			}
		}
		break;
	}

	previous_ = c;
}

Shell::DelayData::DelayData(uint64_t delay_time, delay_function delay_function)
		: delay_time_(delay_time), delay_function_(delay_function) {

}

void Shell::loop_delay() {
	auto *delay_data = reinterpret_cast<Shell::DelayData*>(mode_data_.get());

	if (uuid::get_uptime_ms() >= delay_data->delay_time_) {
		auto function_copy = delay_data->delay_function_;

		mode_ = Mode::NORMAL;
		mode_data_.reset();

		function_copy(*this);

		if (running()) {
			display_prompt();
		}
	}
}

bool Shell::exit_context() {
	if (context_.size() > 1) {
		context_.pop_back();
		return true;
	} else {
		return false;
	}
}

void Shell::enter_password(const __FlashStringHelper *prompt, password_function function) {
	if (mode_ == Mode::NORMAL) {
		mode_ = Mode::PASSWORD;
		mode_data_ = std::make_unique<Shell::PasswordData>(prompt, function);
	}
}

void Shell::delay_for(unsigned long ms, delay_function function) {
	delay_until(uuid::get_uptime_ms() + ms, function);
}

void Shell::delay_until(uint64_t ms, delay_function function) {
	if (mode_ == Mode::NORMAL) {
		mode_ = Mode::DELAY;
		mode_data_ = std::make_unique<Shell::DelayData>(ms, function);
	}
}

void Shell::delete_buffer_word(bool display) {
	size_t pos = line_buffer_.find_last_of(' ');

	if (pos == std::string::npos) {
		line_buffer_.clear();
		if (display) {
			erase_current_line();
			display_prompt();
		}
	} else {
		if (display) {
			erase_characters(line_buffer_.length() - pos);
		}
		line_buffer_.resize(pos);
	}
}

size_t Shell::print(const std::string &data) {
	return write(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
}

size_t Shell::println(const std::string &data) {
	size_t len = print(data);
	len += println();
	return len;
}

size_t Shell::printf(const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	size_t len = vprintf(format, ap);
	va_end(ap);

	return len;
}

size_t Shell::printf(const __FlashStringHelper *format, ...) {
	va_list ap;

	va_start(ap, format);
	size_t len = vprintf(format, ap);
	va_end(ap);

	return len;
}

size_t Shell::printfln(const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	size_t len = vprintf(format, ap);
	va_end(ap);

	len += println();
	return len;
}

size_t Shell::printfln(const __FlashStringHelper *format, ...) {
	va_list ap;

	va_start(ap, format);
	size_t len = vprintf(format, ap);
	va_end(ap);

	len += println();
	return len;
}

size_t Shell::vprintf(const char *format, va_list ap) {
	int len = ::vsnprintf(nullptr, 0, format, ap);
	if (len > 0) {
		std::string text(static_cast<std::string::size_type>(len), '\0');

		::vsnprintf(&text[0], text.capacity() + 1, format, ap);
		return print(text);
	} else {
		return 0;
	}
}

size_t Shell::vprintf(const __FlashStringHelper *format, va_list ap) {
	int len = ::vsnprintf_P(nullptr, 0, reinterpret_cast<PGM_P>(format), ap);
	if (len > 0) {
		std::string text(static_cast<std::string::size_type>(len), '\0');

		::vsnprintf_P(&text[0], text.capacity() + 1, reinterpret_cast<PGM_P>(format), ap);
		return print(text);
	} else {
		return 0;
	}
}

size_t Shell::maximum_command_line_length() const {
	return MAX_COMMAND_LINE_LENGTH;
}

size_t Shell::maximum_log_messages() const {
	return MAX_LOG_MESSAGES;
}

void Shell::erase_current_line() {
	print(F("\033[0G\033[K"));
	prompt_displayed_ = false;
}

void Shell::erase_characters(size_t count) {
	print(std::string(count, '\x08'));
	print(F("\033[K"));
}

void Shell::started() {

}

void Shell::display_banner() {

}

std::string Shell::hostname_text() {
	return "";
}

std::string Shell::context_text() {
	return "";
}

std::string Shell::prompt_prefix() {
	return "";
}

std::string Shell::prompt_suffix() {
	return "$";
}

void Shell::end_of_transmission() {

}

void Shell::stopped() {

}

void Shell::display_prompt() {
	switch (mode_) {
	case Mode::DELAY:
		break;

	case Mode::PASSWORD:
		print(reinterpret_cast<Shell::PasswordData*>(mode_data_.get())->password_prompt_);
		break;

	case Mode::NORMAL:
		std::string hostname = hostname_text();
		std::string context = context_text();

		print(prompt_prefix());
		if (!hostname.empty()) {
			print(hostname);
			print(' ');
		}
		if (!context.empty()) {
			print(context);
			print(' ');
		}
		print(prompt_suffix());
		print(' ');
		print(line_buffer_);
		prompt_displayed_ = true;
		break;
	}
}

void Shell::output_logs() {
	if (!log_messages_.empty()) {
		if (mode_ != Mode::DELAY) {
			erase_current_line();
		}

		while (!log_messages_.empty()) {
			auto &message = log_messages_.front();

			print(uuid::log::format_timestamp_ms(message.content_->uptime_ms, 3));
			printf(F(" %c %lu: [%S] "), uuid::log::format_level_char(message.content_->level), message.id_, message.content_->name);
			println(message.content_->text);

			log_messages_.pop_front();
			::yield();
		}

		display_prompt();
	}
}

void Shell::process_command() {
	std::list<std::string> command_line = parse_line(line_buffer_);

	line_buffer_.clear();
	println();
	prompt_displayed_ = false;

	if (!command_line.empty() && commands_) {
		auto execution = commands_->execute_command(*this, command_line);

		if (execution.error != nullptr) {
			println(execution.error);
		}
	}

	if (running()) {
		display_prompt();
	}
	::yield();
}

void Shell::process_completion() {
	std::list<std::string> command_line = parse_line(line_buffer_);

	if (!command_line.empty() && commands_) {
		auto completion = commands_->complete_command(*this, command_line);
		bool redisplay = false;

		if (!completion.help.empty()) {
			println();
			redisplay = true;

			for (auto &help : completion.help) {
				std::string help_line = format_line(help);

				println(help_line);
			}
		}

		if (!completion.replacement.empty()) {
			if (!redisplay) {
				erase_current_line();
				redisplay = true;
			}

			line_buffer_ = format_line(completion.replacement);
		}

		if (redisplay) {
			display_prompt();
		}
	}

	::yield();
}

void Shell::process_password(bool completed) {
	println();

	auto *password_data = reinterpret_cast<Shell::PasswordData*>(mode_data_.get());
	auto function_copy = password_data->password_function_;

	mode_ = Mode::NORMAL;
	mode_data_.reset();

	function_copy(*this, completed, line_buffer_);
	line_buffer_.clear();

	if (running()) {
		display_prompt();
	}
}

void Shell::invoke_command(std::string line) {
	if (!line_buffer_.empty()) {
		println();
		prompt_displayed_ = false;
	}
	if (!prompt_displayed_) {
		display_prompt();
	}
	line_buffer_ = line;
	print(line_buffer_);
	process_command();
}

std::list<std::string> Shell::parse_line(const std::string &line) {
	std::list<std::string> items;
	bool string_escape_double = false;
	bool string_escape_single = false;
	bool char_escape = false;

	if (!line.empty()) {
		items.emplace_back("");
	}

	for (char c : line) {
		switch (c) {
		case ' ':
			if (string_escape_double || string_escape_single) {
				if (char_escape) {
					items.back().push_back('\\');
					char_escape = false;
				}
				items.back().push_back(' ');
			} else if (char_escape) {
				items.back().push_back(' ');
				char_escape = false;
			} else if (!items.back().empty()) {
				items.emplace_back("");
			}
			break;

		case '"':
			if (char_escape || string_escape_single) {
				items.back().push_back('"');
				char_escape = false;
			} else {
				string_escape_double = !string_escape_double;
			}
			break;

		case '\'':
			if (char_escape || string_escape_double) {
				items.back().push_back('\'');
				char_escape = false;
			} else {
				string_escape_single = !string_escape_single;
			}
			break;

		case '\\':
			if (char_escape) {
				items.back().push_back('\\');
				char_escape = false;
			} else {
				char_escape = true;
			}
			break;

		default:
			if (char_escape) {
				items.back().push_back('\\');
				char_escape = false;
			}
			items.back().push_back(c);
			break;
		}
	}

	return items;
}

std::string Shell::format_line(const std::list<std::string> &items) {
	std::string line;

	line.reserve(maximum_command_line_length());

	for (auto &item : items) {
		if (!line.empty()) {
			line += ' ';
		}

		for (char c : item) {
			switch (c) {
			case ' ':
			case '\"':
			case '\'':
			case '\\':
				line += '\\';
				break;
			}

			line += c;
		}
	}

	return line;
}

} // namespace console

} // namespace uuid
