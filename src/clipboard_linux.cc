#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include "clipboard.h"

namespace {

bool EnsureGtkInitialized() {
    static bool attempted = false;
    static bool initialized = false;
    if (!attempted) {
        attempted = true;
        // Try to initialize without exiting on failure
        int argc = 0;
        char **argv = nullptr;
        initialized = gtk_init_check(&argc, &argv);
    }
    return initialized;
}

std::vector<std::string> splitLines(const std::string &data) {
    std::vector<std::string> lines;
    std::string current;
    for (char ch : data) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (!current.empty()) {
                lines.emplace_back(current);
                current.clear();
            } else {
                lines.emplace_back("");
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        lines.emplace_back(current);
    }
    return lines;
}

std::string joinWithCrlf(const std::vector<std::string> &items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        oss << items[i];
        oss << "\r\n";
    }
    return oss.str();
}

// Clipboard data for text/uri-list via gtk_clipboard_set_with_data
struct UriListData {
    std::string uri_list; // CRLF terminated text/uri-list
    std::string plain_text; // Plain text fallback
};

void clipboard_get_func(GtkClipboard *clipboard, GtkSelectionData *selection_data, guint info, gpointer user_data) {
    (void)clipboard;
    UriListData *payload = static_cast<UriListData *>(user_data);
    if (!payload) {
        return;
    }

    if (info == 0) { // text/uri-list
        const std::string &data = payload->uri_list;
        GdkAtom target = gdk_atom_intern_static_string("text/uri-list");
        gtk_selection_data_set(selection_data, target, 8,
                               reinterpret_cast<const guchar *>(data.data()),
                               static_cast<int>(data.size()));
    } else {
        const std::string &text = payload->plain_text;
        GdkAtom target = gdk_atom_intern_static_string("UTF8_STRING");
        gtk_selection_data_set(selection_data, target, 8,
                               reinterpret_cast<const guchar *>(text.data()),
                               static_cast<int>(text.size()));
    }
}

void clipboard_clear_func(GtkClipboard *clipboard, gpointer user_data) {
    (void)clipboard;
    UriListData *payload = static_cast<UriListData *>(user_data);
    delete payload;
}

} // namespace

std::vector<std::string> ReadFilePaths() {
    std::vector<std::string> result;
    if (!EnsureGtkInitialized()) {
        return result;
    }

    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        return result;
    }

    GdkAtom target = gdk_atom_intern_static_string("text/uri-list");
    GtkSelectionData *sel = gtk_clipboard_wait_for_contents(clipboard, target);
    if (!sel) {
        return result;
    }

    const guchar *data_ptr = gtk_selection_data_get_data(sel);
    gint length = gtk_selection_data_get_length(sel);
    if (!data_ptr || length <= 0) {
        gtk_selection_data_free(sel);
        return result;
    }

    std::string data(reinterpret_cast<const char *>(data_ptr), static_cast<size_t>(length));
    gtk_selection_data_free(sel);

    auto lines = splitLines(data);
    for (const std::string &line : lines) {
        if (line.empty()) {
            continue;
        }
        if (!line.empty() && line[0] == '#') { // comments in text/uri-list
            continue;
        }
        GError *error = nullptr;
        gchar *filename = g_filename_from_uri(line.c_str(), nullptr, &error);
        if (filename) {
            result.emplace_back(filename); // UTF-8
            g_free(filename);
        } else {
            if (error) {
                g_error_free(error);
            }
        }
    }

    return result;
}

void WriteFilePaths(const std::vector<std::string> &file_paths) {
    if (!EnsureGtkInitialized()) {
        return;
    }

    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        return;
    }

    // Build URI list and plain text fallback
    std::vector<std::string> uris;
    uris.reserve(file_paths.size());
    for (const std::string &path : file_paths) {
        GError *error = nullptr;
        gchar *uri = g_filename_to_uri(path.c_str(), nullptr, &error);
        if (uri) {
            uris.emplace_back(uri);
            g_free(uri);
        } else {
            if (error) {
                g_error_free(error);
            }
        }
    }

    UriListData *payload = new UriListData();
    payload->uri_list = joinWithCrlf(uris);

    // Plain text fallback: one path per line
    std::ostringstream plain;
    for (size_t i = 0; i < file_paths.size(); ++i) {
        plain << file_paths[i];
        if (i + 1 < file_paths.size()) {
            plain << '\n';
        }
    }
    payload->plain_text = plain.str();

    GtkTargetEntry targets[] = {
        {const_cast<gchar *>("text/uri-list"), 0, 0},
        {const_cast<gchar *>("UTF8_STRING"), 0, 1},
        {const_cast<gchar *>("STRING"), 0, 1},
    };

    gtk_clipboard_set_with_data(clipboard,
                                targets,
                                static_cast<gint>(sizeof(targets) / sizeof(targets[0])),
                                clipboard_get_func,
                                clipboard_clear_func,
                                payload);

    // Persist clipboard in some environments even if our app exits
    gtk_clipboard_store(clipboard);
}

void ClearClipboard() {
    if (!EnsureGtkInitialized()) {
        return;
    }
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        return;
    }
    gtk_clipboard_clear(clipboard);
}

bool SaveClipboardImageAsJpeg(const std::string &target_path, float compression_factor) {
    if (!EnsureGtkInitialized()) {
        return false;
    }
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        return false;
    }
    GdkPixbuf *pixbuf = gtk_clipboard_wait_for_image(clipboard);
    if (!pixbuf) {
        return false;
    }

    int quality = std::max(0, std::min(100, static_cast<int>(compression_factor * 100.0f)));
    char quality_str[8];
    g_snprintf(quality_str, sizeof(quality_str), "%d", quality);

    GError *error = nullptr;
    gboolean ok = gdk_pixbuf_save(pixbuf, target_path.c_str(), "jpeg", &error, "quality", quality_str, NULL);
    g_object_unref(pixbuf);
    if (!ok && error) {
        g_error_free(error);
    }
    return ok;
}

bool SaveClipboardImageAsPng(const std::string &target_path) {
    if (!EnsureGtkInitialized()) {
        return false;
    }
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        return false;
    }
    GdkPixbuf *pixbuf = gtk_clipboard_wait_for_image(clipboard);
    if (!pixbuf) {
        return false;
    }
    GError *error = nullptr;
    gboolean ok = gdk_pixbuf_save(pixbuf, target_path.c_str(), "png", &error, NULL);
    g_object_unref(pixbuf);
    if (!ok && error) {
        g_error_free(error);
    }
    return ok;
}

bool PutImageIntoClipboard(const std::string &image_path) {
    if (!EnsureGtkInitialized()) {
        return false;
    }
    GError *error = nullptr;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(image_path.c_str(), &error);
    if (!pixbuf) {
        if (error) {
            g_error_free(error);
        }
        return false;
    }
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        g_object_unref(pixbuf);
        return false;
    }
    gtk_clipboard_set_image(clipboard, pixbuf);
    gtk_clipboard_store(clipboard);
    g_object_unref(pixbuf);
    return true;
}

bool ClipboardHasImage() {
    if (!EnsureGtkInitialized()) {
        return false;
    }
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!clipboard) {
        return false;
    }
#if GTK_CHECK_VERSION(2,6,0)
    return gtk_clipboard_wait_is_image_available(clipboard);
#else
    GdkPixbuf *pixbuf = gtk_clipboard_wait_for_image(clipboard);
    if (pixbuf) {
        g_object_unref(pixbuf);
        return true;
    }
    return false;
#endif
}


