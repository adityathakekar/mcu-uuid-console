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
#include <string>
#include <vector>

#include <uuid/log.h>

namespace uuid {

namespace console {

static const char __pstr__logger_name[] __attribute__((__aligned__(sizeof(int)))) PROGMEM = "shell";
uuid::log::Logger Shell::logger_{FPSTR(__pstr__logger_name)};

Shell::Shell(std::shared_ptr<Commands> commands, int context, int flags)
		: context_(context), flags_(flags), commands_(commands) {
	uuid::log::Logger::register_receiver(this, uuid::log::Level::NOTICE);
}

Shell::~Shell() {
	uuid::log::Logger::unregister_receiver(this);
}

void Shell::start() {
	line_buffer_.reserve(maximum_command_line_length());
	display_banner();
	display_prompt();
	flush();
};

void Shell::add_log_message(std::shared_ptr<uuid::log::Message> message) {
	if (log_messages_.size() >= maximum_log_messages()) {
		log_messages_.pop_front();
	}

	log_messages_.emplace_back(message);
}

uuid::log::Level Shell::get_log_level() {
	return uuid::log::Logger::get_log_level(this);
}

void Shell::set_log_level(uuid::log::Level level) {
	uuid::log::Logger::register_receiver(this, level);
}

void Shell::process() {
	output_logs();

	const std::string input = read();

	if (input.empty()) {
		return;
	}

	for (char c : input) {
		switch (c) {
		case '\x03':
			// Interrupt (^C)
			line_buffer_.clear();
			println();
			display_prompt();
			break;

		case '\x04':
			// End of transmission (^D)
			if (line_buffer_.empty()) {
				end_of_transmission();
				println();
				display_prompt();
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

		case '\x0F':
			// Delete line (^U)
			erase_current_line();
			line_buffer_.clear();
			display_prompt();
			break;

		case '\x09':
			// Tab (^I)
			process_completion();
			break;

		case '\x11': {
				// Delete word (^W)
				size_t pos = line_buffer_.find_last_of(' ');

				if (pos == std::string::npos) {
					erase_current_line();
					line_buffer_.clear();
					display_prompt();
				} else {
					erase_characters(line_buffer_.length() - pos);
					line_buffer_.resize(pos);
				}
			}
			break;

		default:
			if (c >= '\x20' && c <= '\x7E') {
				// ASCII text
				if (line_buffer_.length() < maximum_command_line_length()) {
					line_buffer_.push_back(c);
					print(c);
				}
			}
			break;
		}

		previous_ = c;
		flush();
	}
}

void Shell::println() {
	print("\r\n");
	flush();
}

void Shell::println(const char *data) {
	print(data);
	println();
}

void Shell::println(const std::string &data) {
	print(data);
	println();
}

void Shell::println(const __FlashStringHelper *data) {
	print(data);
	println();
}

void Shell::printf(const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	flush();
}

void Shell::printf(const __FlashStringHelper *format, ...) {
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	flush();
}

void Shell::printfln(const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	println();
}

void Shell::printfln(const __FlashStringHelper *format, ...) {
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	println();
}

void Shell::vprintf(const char *format, va_list ap) {
	int len = vsnprintf(nullptr, 0, format, ap);
	if (len > 0) {
		std::string text(static_cast<std::string::size_type>(len), '\0');

		vsnprintf(&text[0], text.capacity() + 1, format, ap);
		print(text);
	}
}

void Shell::vprintf(const __FlashStringHelper *format, va_list ap) {
	int len = vsnprintf_P(nullptr, 0, reinterpret_cast<PGM_P>(format), ap);
	if (len > 0) {
		std::string text(static_cast<std::string::size_type>(len), '\0');

		vsnprintf_P(&text[0], text.capacity() + 1, reinterpret_cast<PGM_P>(format), ap);
		print(text);
	}
}

void Shell::flush() {

}

size_t Shell::maximum_command_line_length() const {
	return MAX_COMMAND_LINE_LENGTH;
}

size_t Shell::maximum_log_messages() const {
	return MAX_LOG_MESSAGES;
}

void Shell::erase_current_line() {
	print(F("\033[0G\033[K"));
}

void Shell::erase_characters(size_t count) {
	print(std::string(count, '\x08'));
	print(F("\033[K"));
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

};

void Shell::display_prompt() {
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
}

void Shell::output_logs() {
	if (!log_messages_.empty()) {
		erase_current_line();

		while (!log_messages_.empty()) {
			auto &message = log_messages_.front();

			print(uuid::log::format_timestamp_ms(3, message->uptime_ms_));
			printf(F(" %c %d: ["), uuid::log::format_level_char(message->level_), message->id_);
			print(message->name_);
			print("] ");
			println(message->text_);

			log_messages_.pop_front();
		}

		display_prompt();
		flush();
	}
}

void Shell::process_command() {
	std::list<std::string> command_line = parse_line(line_buffer_);

	line_buffer_.clear();
	println();

	if (!command_line.empty()) {
		auto execution = commands_->execute_command(*this, context_, flags_, command_line);

		if (execution.error != nullptr) {
			println(execution.error);
		}
	}

	display_prompt();
}

void Shell::process_completion() {
	std::list<std::string> command_line = parse_line(line_buffer_);

	auto completion = commands_->complete_command(*this, context_, flags_, command_line);

	if (!completion.help.empty()) {
		println();

		for (auto &help : completion.help) {
			std::string help_line = unparse_line(help);

			println(help_line);
		}
	}

	if (!completion.replacement.empty()) {
		if (!completion.help.empty()) {
			erase_current_line();
		}

		line_buffer_ = unparse_line(completion.replacement);
	}

	display_prompt();
}

std::list<std::string> Shell::parse_line(const std::string &line) {
	std::list<std::string> items;
	bool escape = false;

	if (!line.empty()) {
		items.emplace_back("");
	}

	for (char c : line) {
		switch (c) {
		case ' ':
			if (escape) {
				items.back().push_back(' ');
				escape = false;
			} else {
				items.emplace_back("");
			}
			break;

		case '\\':
			if (escape) {
				items.back().push_back('\\');
				escape = false;
			} else {
				escape = true;
			}
			break;

		default:
			items.back().push_back(c);
			break;
		}
	}

	return items;
}

std::string Shell::unparse_line(const std::list<std::string> &items) {
	std::string line;

	line.reserve(maximum_command_line_length());

	for (auto &item : items) {
		if (!line.empty()) {
			line += ' ';
		}

		for (char c : item) {
			switch (c) {
			case ' ':
				line += "\\ ";
				break;

			case '\\':
				line += "\\\\";
				break;

			default:
				line += c;
				break;
			}
		}
	}

	return line;
}

} // namespace console

} // namespace uuid