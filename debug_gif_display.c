// Add this BEFORE the #define statements in your original code

// ===================== DEBUGGING ADDITIONS =====================

// Change these paths to actual files on your Mac:
#define GIF_GAME_OVER "/Users/gsm/Documents/KIOSK/gameover.gif"
#define GIF_CONGRATS  "/Users/gsm/Documents/KIOSK/congratulations.gif"

// OR use relative paths if GIFs are in the same directory as your program:
// #define GIF_GAME_OVER "./gameover.gif"
// #define GIF_CONGRATS  "./congratulations.gif"

// ===================== ADD THIS DEBUG FUNCTION =====================

static void debug_check_gif_files(void) {
    g_print("\n========== GIF FILE CHECK ==========\n");
    
    // Check Game Over GIF
    if (access(GIF_GAME_OVER, R_OK) == 0) {
        g_print("✓ Game Over GIF exists: %s\n", GIF_GAME_OVER);
    } else {
        g_print("✗ Game Over GIF NOT FOUND: %s\n", GIF_GAME_OVER);
        g_print("  Error: %s\n", strerror(errno));
    }
    
    // Check Congrats GIF
    if (access(GIF_CONGRATS, R_OK) == 0) {
        g_print("✓ Congrats GIF exists: %s\n", GIF_CONGRATS);
    } else {
        g_print("✗ Congrats GIF NOT FOUND: %s\n", GIF_CONGRATS);
        g_print("  Error: %s\n", strerror(errno));
    }
    
    g_print("====================================\n\n");
}

// ===================== IMPROVED show_fullscreen_gif =====================

static gboolean show_fullscreen_gif(gpointer filename_ptr)
{
    const char *filename = filename_ptr;

    g_print("\n[GIF] ========== LOADING GIF ==========\n");
    g_print("[GIF] Requested file: %s\n", filename);
    
    // Check if file exists
    if (access(filename, R_OK) != 0) {
        g_print("[GIF] ERROR: File not accessible!\n");
        g_print("[GIF] Error: %s\n", strerror(errno));
        g_print("[GIF] =====================================\n\n");
        return FALSE;
    }
    
    g_print("[GIF] File exists and is readable ✓\n");

    /* 1. Mark GIF active */
    gif_playing = TRUE;
    g_print("[GIF] Set gif_playing = TRUE\n");

    /* 2. Cleanup any previous player */
    gif_player_cleanup();
    g_print("[GIF] Cleaned up previous player\n");

    /* 3. Allocate player */
    gif_player = g_new0(GifPlayer, 1);
    g_print("[GIF] Allocated new player\n");

    /* 4. Load animation */
    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    
    if (!gif_player->animation) {
        g_print("[GIF] ❌ FAILED to load animation!\n");
        if (error) {
            g_print("[GIF] Error message: %s\n", error->message);
            g_error_free(error);
        }
        gif_playing = FALSE;
        g_free(gif_player);
        gif_player = NULL;
        g_print("[GIF] =====================================\n\n");
        return FALSE;
    }
    
    g_print("[GIF] Animation loaded successfully ✓\n");
    
    // Check if it's a static image
    gboolean is_static = gdk_pixbuf_animation_is_static_image(gif_player->animation);
    g_print("[GIF] Is static image: %s\n", is_static ? "YES" : "NO");

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    g_print("[GIF] Iterator created\n");

    gif_player->timer = g_timer_new();
    g_print("[GIF] Timer created\n");

    /* 5. Connect draw handler if not already connected */
    if (gif_draw_handler_id == 0) {
        gif_draw_handler_id = g_signal_connect(gif_area, "draw", 
                                               G_CALLBACK(gif_player_draw), NULL);
        g_print("[GIF] Connected draw handler (ID: %lu)\n", gif_draw_handler_id);
    } else {
        g_print("[GIF] Draw handler already connected (ID: %lu)\n", gif_draw_handler_id);
    }

    /* 6. Start animation timer */
    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);
    g_print("[GIF] Animation timer started (ID: %u)\n", gif_player->timeout_id);

    /* 7. Switch stack page */
    g_print("[GIF] Switching to gif_view page...\n");
    gtk_stack_set_visible_child_name(main_stack, "gif_view");
    
    // Verify the switch
    const char *visible = gtk_stack_get_visible_child_name(main_stack);
    g_print("[GIF] Current visible page: %s\n", visible ? visible : "NULL");

    /* 8. Force drawing area to realize and show */
    if (!gtk_widget_get_realized(gif_area)) {
        g_print("[GIF] Drawing area not realized, forcing realize...\n");
        gtk_widget_realize(gif_area);
    }
    
    if (!gtk_widget_get_visible(gif_area)) {
        g_print("[GIF] Drawing area not visible, forcing show...\n");
        gtk_widget_show(gif_area);
    }
    
    // Check allocation
    GtkAllocation alloc;
    gtk_widget_get_allocation(gif_area, &alloc);
    g_print("[GIF] Drawing area allocation: %dx%d at (%d,%d)\n", 
            alloc.width, alloc.height, alloc.x, alloc.y);

    /* 9. Trigger initial draw */
    gtk_widget_queue_draw(gif_area);
    g_print("[GIF] Queued initial draw\n");
    g_print("[GIF] =====================================\n\n");

    return FALSE;
}

// ===================== ADD THIS TO main() =====================
// Add this right after gtk_init():

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    
    // ADD THIS LINE:
    debug_check_gif_files();
    
    // ... rest of your main() code ...
}
