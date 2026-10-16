#include "session_store.h"

#include "redis_client.h"

#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <cstdio>
#include <random>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

namespace {

// A session's fields in one value: each as its byte count, ':', its bytes. A
// value that does not read as `count` of them reads as no session.
void put_field(std::string &out, std::string_view field) {
	out += std::to_string(field.size());
	out += ':';
	out.append(field);
}

std::optional<std::vector<std::string>> read_fields(std::string_view value, size_t count) {
	std::vector<std::string> fields;
	fields.reserve(count);
	while (fields.size() < count) {
		const size_t colon = value.find(':');
		if (colon == std::string_view::npos || colon == 0 || colon > 9)
			return std::nullopt;
		const std::string_view digits = value.substr(0, colon);
		if (!strutil::all_digits(digits))
			return std::nullopt;
		const size_t size =
				static_cast<size_t>(strutil::parse_ulong(std::string(digits)).value_or(0));
		value.remove_prefix(colon + 1);
		if (size > value.size())
			return std::nullopt;
		fields.emplace_back(value.substr(0, size));
		value.remove_prefix(size);
	}
	if (!value.empty())
		return std::nullopt;
	return fields;
}

std::string ttl_ms() {
	return redis_ms(std::chrono::duration_cast<std::chrono::milliseconds>(SessionStore::kTtl));
}

void set(RedisPool &redis, const std::string &key, const std::string &value) {
	redis.command({ "SET", key, value, "PX", ttl_ms() }, RedisResend::IfDropped);
}

std::optional<std::vector<std::string>> fields_of(const RedisReply &reply, size_t count) {
	if (reply.type != RedisReply::Type::String)
		return std::nullopt;
	return read_fields(reply.string, count);
}

std::string login_value(const LoginSession &s) {
	std::string v;
	for (const std::string &f : { std::to_string(s.user_id), s.username, s.pcid, s.nwh, s.nwhandle,
				 s.exp_bits, s.msgbase, s.success, s.failure }) {
		put_field(v, f);
	}
	return v;
}

std::optional<LoginSession> login_of(const std::string &tag, const RedisReply &reply) {
	const auto f = fields_of(reply, 9);
	if (!f)
		return std::nullopt;
	LoginSession s;
	s.session_tag = tag;
	s.user_id = strutil::parse_llong((*f)[0]).value_or(0);
	s.username = (*f)[1];
	s.pcid = (*f)[2];
	s.nwh = (*f)[3];
	s.nwhandle = (*f)[4];
	s.exp_bits = (*f)[5];
	s.msgbase = (*f)[6];
	s.success = (*f)[7];
	s.failure = (*f)[8];
	return s;
}

std::string join_value(const JoinSession &s) {
	std::string v;
	for (const std::string &f : { s.success, s.failure, s.msgbase, s.needexpkey, s.pfid, s.rid })
		put_field(v, f);
	return v;
}

std::optional<JoinSession> join_of(const std::string &tag, const RedisReply &reply) {
	const auto f = fields_of(reply, 6);
	if (!f)
		return std::nullopt;
	JoinSession s;
	s.session_tag = tag;
	s.success = (*f)[0];
	s.failure = (*f)[1];
	s.msgbase = (*f)[2];
	s.needexpkey = (*f)[3];
	s.pfid = (*f)[4];
	s.rid = (*f)[5];
	return s;
}

std::string host_value(const HostSession &s) {
	std::string v;
	for (const std::string &f : { s.host_key, s.success, s.pfid })
		put_field(v, f);
	return v;
}

std::optional<HostSession> host_of(const std::string &tag, const RedisReply &reply) {
	const auto f = fields_of(reply, 3);
	if (!f)
		return std::nullopt;
	HostSession s;
	s.session_tag = tag;
	s.host_key = (*f)[0];
	s.success = (*f)[1];
	s.pfid = (*f)[2];
	return s;
}

} // namespace

std::string SessionStore::generate_tag(const std::string &dll_name) {
	OsRandom gen; // the OS CSPRNG (base/os_random)
	std::uniform_int_distribution<int> rand5(0, 99999);
	std::uniform_int_distribution<uint32_t> hex8;
	char buf[160];
	std::snprintf(buf, sizeof(buf), "NWServer:%s:SESSIONTAG:%d:%08x",
	              dll_name.c_str(), rand5(gen), hex8(gen));
	return buf;
}

void SessionStore::put_login(const std::string &tag, const LoginSession &session) {
	set(redis_, redis_.key("login:" + tag), login_value(session));
}

std::optional<LoginSession> SessionStore::get_login(const std::string &tag) {
	return login_of(
			tag, redis_.command({ "GET", redis_.key("login:" + tag) }, RedisResend::IfDropped));
}

void SessionStore::erase_login(const std::string &tag) {
	redis_.command({ "DEL", redis_.key("login:" + tag) }, RedisResend::IfDropped);
}

void SessionStore::put_join(const std::string &tag, const JoinSession &session) {
	set(redis_, redis_.key("join:" + tag), join_value(session));
}

std::optional<JoinSession> SessionStore::get_join(const std::string &tag) {
	return join_of(
			tag, redis_.command({ "GET", redis_.key("join:" + tag) }, RedisResend::IfDropped));
}

void SessionStore::put_host(const std::string &tag, const HostSession &session) {
	set(redis_, redis_.key("host:" + tag), host_value(session));
}

std::optional<HostSession> SessionStore::get_host(const std::string &tag) {
	return host_of(
			tag, redis_.command({ "GET", redis_.key("host:" + tag) }, RedisResend::IfDropped));
}

std::optional<HostSession> SessionStore::take_host(const std::string &tag) {
	return host_of(
			tag, redis_.command({ "GETDEL", redis_.key("host:" + tag) }, RedisResend::Never));
}

} // namespace opennova::novaworld_server
