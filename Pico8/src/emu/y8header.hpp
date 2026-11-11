#include <string_view>

// Header injected at the start of every cart in Lua
static constexpr std::string_view app_header =
	R"(
all = function(t)
	if t == nil or type(t) ~= 'table' or #t == 0 then
		return function() end
	end

	local i = 1
	local prev = nil

	return function()
		if t[i] == prev then
			i = i + 1
		end

		while t[i] == nil and i <= #t do
			i = i + 1
		end

		prev = t[i]

		return prev
	end
end

inext = function(t, idx)
	if idx == nil then idx = 1 else idx = idx + 1 end
	local x = t[idx]
	if x == nil then return nil end
	return idx, x
end

count = function(t, v)
	if t == nil or type(t) ~= 'table' then return 0 end
	local n = 0
	if v == nil then
		for i = 1, #t do
			if t[i] ~= nil then
				n = n + 1
			end
		end
	else
		for i = 1, #t do
			if t[i] == v then
				n = n + 1
			end
		end
	end
	return n
end

__p8_env = function(t)
	if type(t) ~= 'table' then return t end
	if getmetatable(t) == nil then
		setmetatable(t, {__index = _G})
	end
	return t
end

_gc();
)";