# Freeciv GTK 3.22 Animation Fixes Roadmap

## Problem Statement
Visual artifacts (outdated map regions) occur during battle animations and unit movements in GTK 3.22 client. Issues are "occasional" suggesting timing-dependent bugs and edge cases in the drawing pipeline.

## Root Cause Analysis Summary

### High-Priority Issues (Definite Bugs)
1. **Battle Animation Missing Canvas Restoration** (`mapview_common.c:267-323`)
   - `battle_animation()` draws virtual units but never restores previous frame
   - Compare: `movement_animation()` and `explosion_animation()` correctly call `update_map_canvas(anim->old_x, anim->old_y, ...)`
   - Impact: Virtual unit sprites accumulate, leaving stale content on canvas

2. **Canvas Surface Synchronization Missing** (`gui-gtk-3.22/canvas.c`)
   - `canvas_copy()` reads from source surface without `cairo_surface_flush(src->surface)`
   - No `cairo_surface_mark_dirty()` on destination surfaces after drawing
   - Impact: Cairo's internal caching may return stale pixel data, causing corruption

3. **Animation ID Mismatch for Unit Suppression** (`mapview_common.c:2575`)
   - Battle animations set `anim->id = -1`, but `put_one_tile()` only suppresses units where `punit->id == anim->id`
   - Impact: Real units draw alongside virtual animated units during combat

### Medium-Priority Issues (Likely Contributors)
4. **Dirty Rectangle Recursion Risk** (`mapview_common.c:3074-3187`)
   - `unqueue_mapview_updates()` processes tile lists but may encounter reentrancy
   - `overview_update_tile()` (90% of runtime per FIXME) could queue new updates

5. **Partial Redraw Edge Cases** (`mapview_common.c:854-949`)
   - `can_do_cached_drawing()` has complex logic for wrapping/isometric maps
   - When disabled (zoom, large mapviews), fallback path may miss overlapping regions

6. **Unit Sprite Overlap in Isometric View**
   - Units extend beyond tile boundaries (`tileset_unit_height > tileset_tile_height`)
   - Dirty rectangles use tile dimensions, not unit sprite dimensions
   - Impact: Adjacent tile overlaps leave stale pixels

### Low-Priority Issues (Possible Under Specific Conditions)
7. **Main-Thread Blocking During Packet Processing**
   - Heavy computation in packet handlers delays GTK draw callbacks
   - Animation timer continues, causing frame jumps when draw finally occurs

8. **First-Frame Sprite Loading Stalls**
   - Lazy sprite loading causes synchronous I/O during first draw of new unit types
   - Could cause incomplete frames if load fails or is slow

9. **GTK 3.22 Drawing API Inconsistency**
   - `pixmap_put_overlay_tile()` uses `gdk_window_begin_draw_frame()` API
   - Canvas functions bypass this, drawing directly to image surfaces
   - Potential platform-specific Cairo behavior differences

## Implementation Plan

### Phase 1: Critical Bug Fixes (Highest Impact, Lowest Risk)
**Goal**: Fix fundamental drawing bugs causing most artifacts

**1.1 Add Canvas Restoration to `battle_animation()`**
- Location: `mapview_common.c:267-323`
- Add `update_map_canvas(anim->old_x, anim->old_y, ...)` before drawing virtual units
- Set `anim->old_x/old_y` in `decrease_unit_hp_smooth()`
- **Tradeoff**: +1 `update_map_canvas()` call per animation frame. Negligible performance impact.

**1.2 Proper Surface Synchronization in `canvas_copy()`**
- Location: `gui-gtk-3.22/canvas.c:76-103`
- Add `cairo_surface_flush(src->surface)` before reading
- Add `cairo_surface_mark_dirty(dest->surface)` after writing
- **Tradeoff**: Slight performance cost from extra Cairo calls. Eliminates platform-dependent corruption.

**1.3 Set Valid Animation ID for Battle Animations**
- Location: `mapview_common.c:2575` in `decrease_unit_hp_smooth()`
- Set `anim->id = winning_unit->id` (or loser unit ID)
- **Tradeoff**: Suppresses real unit drawing during battle. May cause brief missing unit if animation ends before tile refresh.

**Expected Outcome**: 70-80% artifact reduction, negligible performance impact.

**Implementation Status (2026-04-07)**:
- ✅ **1.1 Battle animation canvas restoration**: Added `update_map_canvas()` calls before drawing virtual units, stores tile origin for cleanup
- ✅ **1.2 Canvas surface synchronization**: Added `cairo_surface_flush(src->surface)` and `cairo_surface_mark_dirty(dest->surface)` in `canvas_copy()`
- ✅ **1.3 Valid animation ID**: Initial Phase 1 change switched battle suppression from `-1` to the winning unit ID
- ✅ **2.1 Unit dimensions for battle redraw bounds**: Battle animation now uses unit-sized regions for redraw, restoration, and cleanup rather than only tile-sized regions
- ✅ **2.2 Suppress both combatants during battle animation**: Battle animations now suppress both real units while the virtual winner/loser pair is drawn
- ✅ **2.3 Track cleanup for both battle participants**: Battle animations now remember and restore both affected map regions on completion
- ✅ **2.4 Reentrant-safe `unqueue_mapview_updates()`**: Nested update requests are drained in batches without dropping newly queued work
- ✅ **2.5 Explosion animation uses unit-sized dirty rects**: Changed explosion anim (used on unit death) to use unit width/height for dirty/update_map_canvas so HP bar remnants are fully cleared (previously used tile size only)
- ✅ **2.6 Consistent post-offset draw positions in iso view**: Battle and explosion animations now compute `draw_x`/`draw_y` (including isometric unit offset) and use it uniformly for restore/dirty/cleanup. Fixes side-vs-top attack direction artifacts.

### Phase 2: Secondary Improvements (Moderate Impact, Low Risk)
**Goal**: Fix overlapping sprite issues and improve dirty region handling

**2.1 Use Unit Dimensions for Battle Animation Bounds**
- Update battle animation redraw, restoration, and cleanup bounds to use `tileset_unit_width/height`
- `dirty_rect()` alone is not enough; `update_map_canvas()` and final cleanup must use the same larger area
- **Tradeoff**: Larger dirty regions → slightly more redraw work. Fixes overlap and stale HP-bar artifacts.

**2.2 Suppress Both Combatants During Battle Animation**
- Suppress both real units while the virtual battle sprites are being drawn
- The original single-ID suppression could still let the enemy unit redraw underneath the animation
- **Tradeoff**: Slightly broader suppression during battle frames, but avoids double-drawing and stale overlays.

**2.3 Track Cleanup Regions for Both Battle Participants**
- Store and restore both battle tile origins on animation completion
- This replaces the earlier narrower idea of only initializing generic `old_x/old_y`
- **Tradeoff**: Minimal state increase in the animation struct, but more reliable final cleanup.

**2.4 Add Reentrant-Safe Batching to `unqueue_mapview_updates()`**
- Add static `in_unqueue` guard to prevent recursive processing
- Drain newly queued updates in a loop after the current batch instead of clearing them accidentally
- **Tradeoff**: Slightly more control-flow complexity, but avoids dropped updates during redraw-heavy combat.

**Expected Outcome**: Additional 10-15% artifact reduction, minor performance impact (2-5%).

### Phase 3: Optimization & Edge Cases (Lower Impact, Higher Risk)
**Goal**: Address performance bottlenecks and complex edge cases

**3.1 Batch Overview Updates** (Address FIXME at line 3162)
- Collect tiles needing overview updates, process once after map updates
- **Tradeoff**: Significant refactoring, risk of breaking overview sync.

**3.2 Improve `can_do_cached_drawing()` Logic**
- Audit edge cases for wrapping isometric maps at specific zoom levels
- **Tradeoff**: Complex math, risk of introducing new bugs.

**3.3 Preload Game-Object Sprites**
- Load all unit/city/terrain sprites after ruleset receipt, before gameplay
- **Tradeoff**: Increased memory usage, longer initial load time.

**Expected Outcome**: Final 5-10% artifact reduction, moderate performance impact (5-10%).

## Testing Strategy

1. **Automated Tests**: Add unit tests for animation restoration logic
2. **Visual Regression**: Capture screenshots before/after battle animations
3. **Stress Testing**: Rapid unit movements during combat animations
4. **Platform Coverage**: Test on Linux (GTK 3.22) and macOS (different Cairo backend)
5. **Performance Monitoring**: Measure FPS before/after each phase

## Success Metrics

- **Primary**: No visual artifacts after battle animations complete
- **Secondary**: Smooth unit movement animations without stale content
- **Tertiary**: No performance regression (>10% FPS drop)

## Timeline & Prioritization

1. **Immediate (Week 1)**: Implement Phase 1 fixes, basic testing
2. **Short-term (Week 2)**: Implement Phase 2 fixes, comprehensive testing
3. **Medium-term (Month 1)**: Implement Phase 3 optimizations, performance testing
4. **Long-term**: Continuous monitoring and edge case fixes

## Risk Mitigation

- **Rollback Plan**: Each phase is independently revertible
- **A/B Testing**: Compare fixed vs original builds side-by-side
- **Incremental Deployment**: Test on development builds before stable release

## Documentation

- Update code comments for fixed functions
- Add developer notes about Cairo surface synchronization requirements
- Document animation system architecture for future maintenance

## Related Files

- `client/mapview_common.c` - Core animation logic
- `client/gui-gtk-3.22/canvas.c` - Cairo drawing operations
- `client/gui-gtk-3.22/mapview.c` - GTK drawing callbacks
- `client/tilespec.c` - Sprite loading and management
- `client/control.c` - Unit movement handling

---
*Last Updated: April 7, 2026*
*Status: Phase 2 Implemented (2026-04-07)*
