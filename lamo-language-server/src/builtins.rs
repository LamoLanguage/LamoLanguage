//! Language builtins — single source of truth extracted from the official
//! compiler (`src/builtins.h` in LamoLanguage). Lang/GUI/HTTP builtins are
//! available globally; std builtins are wrapped by the std/* modules.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BuiltinCategory {
    Lang,
    Gui,
    Http,
    Std,
}

#[derive(Debug, Clone)]
pub struct BuiltinInfo {
    pub name: &'static str,
    pub arity: usize,
    pub category: BuiltinCategory,
    pub ret: &'static str,
    pub doc: &'static str,
}

pub static BUILTINS: &[BuiltinInfo] = &[
    BuiltinInfo {
        name: "print",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Print a value to stdout (no trailing newline). Returns 1.\nBooleans print as true/false; arrays and structs print in literal form.",
    },
    BuiltinInfo {
        name: "input",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Print the prompt and read one line of input. Returns the input as a string value.",
    },
    BuiltinInfo {
        name: "input_int",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Print the prompt and read an integer from stdin.",
    },
    BuiltinInfo {
        name: "input_str",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "string",
        doc: "Print the prompt and read a line of input as a string.",
    },
    BuiltinInfo {
        name: "isnumber",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "bool",
        doc: "Return true if the value is a number (int or float).",
    },
    BuiltinInfo {
        name: "isstring",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "bool",
        doc: "Return true if the value is a string.",
    },
    BuiltinInfo {
        name: "isarray",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "bool",
        doc: "Return true if the value is an array.",
    },
    BuiltinInfo {
        name: "exit",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Terminate the program with the given exit code.",
    },
    BuiltinInfo {
        name: "abs",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "mirror of arg0",
        doc: "Absolute value. Works on int and float; returns the same type as the argument.",
    },
    BuiltinInfo {
        name: "len",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Return the element count of an array.",
    },
    BuiltinInfo {
        name: "push",
        arity: 2,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Append a value to an array: push(arr, x). Equivalent to arr.push(x).",
    },
    BuiltinInfo {
        name: "pop",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "value",
        doc: "Remove and return the last element of an array: pop(arr). Equivalent to arr.pop().",
    },
    BuiltinInfo {
        name: "gc_collect",
        arity: 0,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Run a full mark-sweep GC cycle. Returns the number of live allocations.",
    },
    BuiltinInfo {
        name: "gc_set_threshold",
        arity: 1,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Set the auto-GC threshold in bytes. 0 disables automatic collection.",
    },
    BuiltinInfo {
        name: "gc_heap_size",
        arity: 0,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Return live heap bytes after the last collection.",
    },
    BuiltinInfo {
        name: "gc_heap_count",
        arity: 0,
        category: BuiltinCategory::Lang,
        ret: "int",
        doc: "Return the live allocation count after the last collection.",
    },
    BuiltinInfo {
        name: "gui_open",
        arity: 3,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Open a native window (Win32/X11): gui_open(title, width, height).",
    },
    BuiltinInfo {
        name: "gui_should_close",
        arity: 0,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Return 1 when the user requested to close the window.",
    },
    BuiltinInfo {
        name: "gui_begin_frame",
        arity: 3,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Begin a frame and clear the window with an RGB color.",
    },
    BuiltinInfo {
        name: "gui_draw_rect",
        arity: 7,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Draw a rectangle: gui_draw_rect(x, y, w, h, r, g, b).",
    },
    BuiltinInfo {
        name: "gui_draw_text",
        arity: 6,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Draw text: gui_draw_text(text, x, y, size, r, g, b).",
    },
    BuiltinInfo {
        name: "gui_end_frame",
        arity: 0,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Present the current frame.",
    },
    BuiltinInfo {
        name: "gui_close",
        arity: 0,
        category: BuiltinCategory::Gui,
        ret: "int",
        doc: "Close the window and release resources.",
    },
    BuiltinInfo {
        name: "http_route",
        arity: 2,
        category: BuiltinCategory::Http,
        ret: "int",
        doc: "Register a route handler: http_route(\"/path\", fnName).",
    },
    BuiltinInfo {
        name: "http_serve",
        arity: 1,
        category: BuiltinCategory::Http,
        ret: "int",
        doc: "Start the HTTP server on the given port (blocks).",
    },
    BuiltinInfo {
        name: "http_serve_once",
        arity: 1,
        category: BuiltinCategory::Http,
        ret: "int",
        doc: "Serve a single HTTP request, then return.",
    },
];


// ---------------------------------------------------------------------------
// std runtime builtins (BUILTIN_STD in the official compiler's builtins.h).
// These power the std/* modules; user code normally reaches them through
// the namespaced std imports, but std/*.lamo sources call them directly,
// so the LSP must know them to avoid false positives inside std.
// ---------------------------------------------------------------------------

pub static STD_BUILTINS: &[BuiltinInfo] = &[
    BuiltinInfo { name: "__lamo_std_math_sqrt", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level sqrt (wrapped by std.math.sqrt)." },
    BuiltinInfo { name: "__lamo_std_math_pow", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level pow (wrapped by std.math.pow)." },
    BuiltinInfo { name: "__lamo_std_math_sin", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level sin (wrapped by std.math.sin)." },
    BuiltinInfo { name: "__lamo_std_math_cos", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level cos (wrapped by std.math.cos)." },
    BuiltinInfo { name: "__lamo_std_math_tan", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level tan (wrapped by std.math.tan)." },
    BuiltinInfo { name: "__lamo_std_math_floor", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level floor (wrapped by std.math.floor)." },
    BuiltinInfo { name: "__lamo_std_math_ceil", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level ceil (wrapped by std.math.ceil)." },
    BuiltinInfo { name: "__lamo_std_math_round", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level round (wrapped by std.math.round)." },
    BuiltinInfo { name: "__lamo_std_math_min", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level min (wrapped by std.math.min)." },
    BuiltinInfo { name: "__lamo_std_math_max", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level max (wrapped by std.math.max)." },
    BuiltinInfo { name: "__lamo_std_math_clamp", arity: 3, category: BuiltinCategory::Std, ret: "int", doc: "C-level clamp (wrapped by std.math.clamp)." },
    BuiltinInfo { name: "__lamo_std_str_length", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level string length (wrapped by std.string.length)." },
    BuiltinInfo { name: "__lamo_std_str_upper", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level uppercase (wrapped by std.string.upper)." },
    BuiltinInfo { name: "__lamo_std_str_lower", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level lowercase (wrapped by std.string.lower)." },
    BuiltinInfo { name: "__lamo_std_str_starts_with", arity: 2, category: BuiltinCategory::Std, ret: "bool", doc: "C-level prefix check (wrapped by std.string.startsWith)." },
    BuiltinInfo { name: "__lamo_std_str_ends_with", arity: 2, category: BuiltinCategory::Std, ret: "bool", doc: "C-level suffix check (wrapped by std.string.endsWith)." },
    BuiltinInfo { name: "__lamo_std_str_contains", arity: 2, category: BuiltinCategory::Std, ret: "bool", doc: "C-level substring check (wrapped by std.string.contains)." },
    BuiltinInfo { name: "__lamo_std_str_index_of", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level indexOf (wrapped by std.string.indexOf)." },
    BuiltinInfo { name: "__lamo_std_str_trim", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level trim (wrapped by std.string.trim)." },
    BuiltinInfo { name: "__lamo_std_str_substring", arity: 3, category: BuiltinCategory::Std, ret: "string", doc: "C-level substring (wrapped by std.string.substring)." },
    BuiltinInfo { name: "__lamo_std_str_replace", arity: 3, category: BuiltinCategory::Std, ret: "string", doc: "C-level replace (wrapped by std.string.replace)." },
    BuiltinInfo { name: "__lamo_std_str_split", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level split (wrapped by std.string.split)." },
    BuiltinInfo { name: "__lamo_std_str_char_at", arity: 2, category: BuiltinCategory::Std, ret: "string", doc: "C-level charAt (wrapped by std.string.charAt)." },
    BuiltinInfo { name: "__lamo_std_str_repeat", arity: 2, category: BuiltinCategory::Std, ret: "string", doc: "C-level repeat (wrapped by std.string.repeat)." },
    BuiltinInfo { name: "__lamo_std_path_join", arity: 2, category: BuiltinCategory::Std, ret: "string", doc: "C-level path join (wrapped by std.path.join)." },
    BuiltinInfo { name: "__lamo_std_path_parent", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level path parent (wrapped by std.path.parent)." },
    BuiltinInfo { name: "__lamo_std_path_filename", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level path filename (wrapped by std.path.filename)." },
    BuiltinInfo { name: "__lamo_std_path_extension", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level path extension (wrapped by std.path.extension)." },
    BuiltinInfo { name: "__lamo_std_path_absolute", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level absolute path (wrapped by std.path.absolute)." },
    BuiltinInfo { name: "__lamo_std_path_normalize", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level normalize (wrapped by std.path.normalize)." },
    BuiltinInfo { name: "__lamo_std_fs_exists", arity: 1, category: BuiltinCategory::Std, ret: "bool", doc: "C-level file exists (wrapped by std.fs.exists)." },
    BuiltinInfo { name: "__lamo_std_fs_is_file", arity: 1, category: BuiltinCategory::Std, ret: "bool", doc: "C-level is-file (wrapped by std.fs.isFile)." },
    BuiltinInfo { name: "__lamo_std_fs_is_dir", arity: 1, category: BuiltinCategory::Std, ret: "bool", doc: "C-level is-dir (wrapped by std.fs.isDir)." },
    BuiltinInfo { name: "__lamo_std_fs_read_text", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level read text (wrapped by std.fs.readText)." },
    BuiltinInfo { name: "__lamo_std_fs_write_text", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level write text (wrapped by std.fs.writeText)." },
    BuiltinInfo { name: "__lamo_std_fs_append_text", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level append text (wrapped by std.fs.appendText)." },
    BuiltinInfo { name: "__lamo_std_fs_delete", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level delete (wrapped by std.fs.delete)." },
    BuiltinInfo { name: "__lamo_std_fs_create_dir", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level mkdir (wrapped by std.fs.createDir)." },
    BuiltinInfo { name: "__lamo_std_fs_remove_dir", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level rmdir (wrapped by std.fs.removeDir)." },
    BuiltinInfo { name: "__lamo_std_fs_copy", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level copy (wrapped by std.fs.copy)." },
    BuiltinInfo { name: "__lamo_std_fs_move", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level move (wrapped by std.fs.move)." },
    BuiltinInfo { name: "__lamo_std_fs_list_files", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level list files (wrapped by std.fs.listFiles)." },
    BuiltinInfo { name: "__lamo_std_fs_size", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level file size (wrapped by std.fs.size)." },
    BuiltinInfo { name: "__lamo_std_env_get", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level getenv (wrapped by std.env.get)." },
    BuiltinInfo { name: "__lamo_std_env_set", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level setenv (wrapped by std.env.set)." },
    BuiltinInfo { name: "__lamo_std_env_remove", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level unsetenv (wrapped by std.env.remove)." },
    BuiltinInfo { name: "__lamo_std_os_name", arity: 0, category: BuiltinCategory::Std, ret: "string", doc: "C-level OS name (wrapped by std.os.name)." },
    BuiltinInfo { name: "__lamo_std_os_arch", arity: 0, category: BuiltinCategory::Std, ret: "string", doc: "C-level OS arch (wrapped by std.os.arch)." },
    BuiltinInfo { name: "__lamo_std_os_cpu_count", arity: 0, category: BuiltinCategory::Std, ret: "int", doc: "C-level CPU count (wrapped by std.os.cpuCount)." },
    BuiltinInfo { name: "__lamo_std_os_home", arity: 0, category: BuiltinCategory::Std, ret: "string", doc: "C-level home dir (wrapped by std.os.home)." },
    BuiltinInfo { name: "__lamo_std_os_temp_dir", arity: 0, category: BuiltinCategory::Std, ret: "string", doc: "C-level temp dir (wrapped by std.os.tempDir)." },
    BuiltinInfo { name: "__lamo_std_time_now", arity: 0, category: BuiltinCategory::Std, ret: "int", doc: "C-level wall clock (wrapped by std.time.now)." },
    BuiltinInfo { name: "__lamo_std_time_timestamp", arity: 0, category: BuiltinCategory::Std, ret: "int", doc: "C-level timestamp (wrapped by std.time.timestamp)." },
    BuiltinInfo { name: "__lamo_std_time_sleep", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level sleep (wrapped by std.time.sleep)." },
    BuiltinInfo { name: "__lamo_std_time_monotonic", arity: 0, category: BuiltinCategory::Std, ret: "int", doc: "C-level monotonic clock (wrapped by std.time.monotonic)." },
    BuiltinInfo { name: "__lamo_std_process_pid", arity: 0, category: BuiltinCategory::Std, ret: "int", doc: "C-level pid (wrapped by std.process.pid)." },
    BuiltinInfo { name: "__lamo_std_process_run", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level run (wrapped by std.process.run)." },
    BuiltinInfo { name: "__lamo_std_process_exec", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level exec (wrapped by std.process.exec)." },
    BuiltinInfo { name: "__lamo_std_process_exit", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level exit (wrapped by std.process.exit)." },
    BuiltinInfo { name: "__lamo_std_random_seed", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level seed (wrapped by std.random.seed)." },
    BuiltinInfo { name: "__lamo_std_random_int", arity: 2, category: BuiltinCategory::Std, ret: "int", doc: "C-level random int (wrapped by std.random.int)." },
    BuiltinInfo { name: "__lamo_std_random_float", arity: 0, category: BuiltinCategory::Std, ret: "int", doc: "C-level random float (wrapped by std.random.float)." },
    BuiltinInfo { name: "__lamo_std_random_bool", arity: 0, category: BuiltinCategory::Std, ret: "bool", doc: "C-level random bool (wrapped by std.random.bool)." },
    BuiltinInfo { name: "__lamo_std_random_choice", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level random choice (wrapped by std.random.choice)." },
    BuiltinInfo { name: "__lamo_std_random_shuffle", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level shuffle (wrapped by std.random.shuffle)." },
    BuiltinInfo { name: "__lamo_std_io_println", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level println (wrapped by std.io.println)." },
    BuiltinInfo { name: "__lamo_std_io_eprint", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level stderr print (wrapped by std.io.eprint)." },
    BuiltinInfo { name: "__lamo_std_io_read_line", arity: 0, category: BuiltinCategory::Std, ret: "string", doc: "C-level read line (wrapped by std.io.readLine)." },
    BuiltinInfo { name: "__lamo_std_io_write", arity: 1, category: BuiltinCategory::Std, ret: "int", doc: "C-level write (wrapped by std.io.write)." },
    BuiltinInfo { name: "__lamo_std_net_http_get", arity: 1, category: BuiltinCategory::Std, ret: "string", doc: "C-level HTTP GET (wrapped by std.net.get)." },
    BuiltinInfo { name: "__lamo_std_net_http_post", arity: 2, category: BuiltinCategory::Std, ret: "string", doc: "C-level HTTP POST (wrapped by std.net.post)." },
];

pub fn lookup_std(name: &str) -> bool {
    STD_BUILTINS.iter().any(|b| b.name == name)
}

pub fn lookup_any(name: &str) -> Option<&'static BuiltinInfo> {
    lookup(name).or_else(|| STD_BUILTINS.iter().find(|b| b.name == name))
}

pub fn lookup(name: &str) -> Option<&'static BuiltinInfo> {
    BUILTINS.iter().find(|b| b.name == name)
}

pub fn is_builtin(name: &str) -> bool {
    lookup(name).is_some() || lookup_std(name)
}

/// Array methods (SPEC §9.2): `arr.push(x)`, `arr.pop()`, `arr.len()`.
pub static ARRAY_METHODS: &[(&str, usize, &str)] = &[
    ("push", 1, "Append a value and return it (mutates the array)."),
    ("pop", 0, "Remove and return the last element."),
    ("len", 0, "Return the number of elements."),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lookup_works() {
        assert!(lookup("print").is_some());
        assert_eq!(lookup("print").unwrap().arity, 1);
        assert!(lookup("__lamo_std_math_sqrt").is_none()); // internal name, wrapped by std.math
        assert!(!is_builtin("sqrt"));
    }

    #[test]
    fn array_methods() {
        assert!(ARRAY_METHODS.iter().any(|(n, _, _)| *n == "push"));
    }
}
