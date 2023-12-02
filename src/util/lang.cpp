#pragma once
#include "lang.h"
#include "Settings.h"
#include "util.h"
#include <fmt/format.h>

inih::INIReader * ft = NULL;

std::map<int, std::string> lang_db;
std::map<std::string, std::string> lang_db_str;

std::string get_localized_string(int id)
{
	if (ft == NULL)
	{
		set_localize_lang("EN");
	}

	std::map<int, std::string>::iterator itr = lang_db.find(id);

	if (itr == lang_db.end())
	{
		std::string value = ft->Get<std::string>(g_settings.language, fmt::format("LANG_{:04}", id), fmt::format("NO LANG_{:04}", id));
		replaceAll(value, "\\n", "\n");
		lang_db[id] = value;
		return value;
	}

	return itr->second;
}

std::string get_localized_string(const std::string & str_id)
{
	if (ft == NULL)
	{
		set_localize_lang("EN");
	}

	std::map<std::string, std::string>::iterator itr = lang_db_str.find(str_id);

	if (itr == lang_db_str.end())
	{
		std::string value = ft->Get<std::string>(g_settings.language,str_id, fmt::format("NO {}", str_id));
		replaceAll(value, "\\n", "\n");
		lang_db_str[str_id] = value;
		return value;
	}

	return itr->second;
}

void set_localize_lang(std::string lang)
{
	if (ft != NULL)
	{
		delete ft;
	}

	ft = new inih::INIReader(g_config_dir + "language.ini");

	g_settings.language = lang;
	lang_db.clear();
	lang_db_str.clear();
}