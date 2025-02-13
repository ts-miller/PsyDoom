//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing code for the new native Vulkan renderer: things/sprites
//------------------------------------------------------------------------------------------------------------------------------------------

#if PSYDOOM_VULKAN_RENDERER

#include "rv_sprites.h"

#include "Doom/Base/i_texcache.h"
#include "Doom/Game/doomdata.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/p_setup.h"
#include "Doom/Game/sprinfo.h"
#include "Doom/Renderer/r_data.h"
#include "Doom/Renderer/r_local.h"
#include "Doom/Renderer/r_main.h"
#include "PsyDoom/Vulkan/VDrawing.h"
#include "PsyDoom/Vulkan/VTypes.h"
#include "rv_bsp.h"
#include "rv_data.h"
#include "rv_main.h"
#include "rv_utils.h"

#include <vector>

//------------------------------------------------------------------------------------------------------------------------------------------
// Describes a piece of a sprite.
// Sprites get split up by the infinitely high walls defined by BSP tree splits, on subsector boundaries.
// Splitting sprites allows the individual pieces to be ordered correctly in relation to world geometry and other sprites.
// This method is used by the recent Doom64 re-release, and is also suggested by John Carmack in his 1997 Doom source release notes.
//------------------------------------------------------------------------------------------------------------------------------------------
struct SpriteFrag {
    int32_t         nextSubsecFragIdx;                  // Index of the next sprite fragment in the linked list for the subsector
    float           depth;                              // Depth of the sprite fragment
    float           x1, z1;                             // 1st billboard endpoint: xz world position
    float           x2, z2;                             // 2nd billboard endpoint: xz world position
    float           yt, yb;                             // World top and bottom 'y' position
    float           ul, ur;                             // 'U' Texture coordinate for left and right side of the sprite
    float           vt, vb;                             // 'V' Texture coordinate for top and bottom of the sprite
    VPipelineType   drawPipeline;                       // Which pipeline to render the sprite with
    uint8_t         colR, colG, colB;                   // Color to shade the sprite with
    uint8_t         stMulR, stMulG, stMulB, stMulA;     // Semi-transparency multiply vector for semi-transparent pixels
    uint16_t        texWinX, texWinY;                   // Sprite texture window location
    uint16_t        texWinW, texWinH;                   // Sprite texture window size
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Describes a very short 2D line, crossing over a candidate sprite spliting BSP node.
// Used for testing whether a split can occur by colliding the line against level geometry.
//------------------------------------------------------------------------------------------------------------------------------------------
struct SplitTestLine {
    float x1, z1;
    float x2, z2;
    float y;            // At what height is the test line: used to decide whether 2-sided lines should be treated as blocking
};

// All of the sprite fragments to be drawn in this frame
static std::vector<SpriteFrag> gRvSpriteFrags;

// The sprite fragment linked list for each draw subsector (-1 if no sprite fragments)
static std::vector<int32_t> gRvDrawSubsecSprFrags;

// Depth sorted sprite fragments to be drawn for the current draw subsector.
// This temporary list is re-used for each subsector to avoid allocations.
static std::vector<const SpriteFrag*> gRvSortedFrags;

// XYZ position for the current thing which is having sprite fragments generated
static float gSpriteFragThingPos[3];

//------------------------------------------------------------------------------------------------------------------------------------------
// Get and cache the texture to use for the given thing and sprite frame, and get whether it is flipped.
// This code is copied more or less directly from 'R_DrawSubsectorSprites'.
//------------------------------------------------------------------------------------------------------------------------------------------
static texture_t& RV_CacheThingSpriteFrame(
    const fixed_t thingX,
    const fixed_t thingY,
    const angle_t thingAngle,
    const spriteframe_t& frame,
    bool& bFlipSrite
) noexcept {
    // Decide on which sprite lump to use and whether the sprite is flipped.
    // If the frame supports rotations then decide on the exact orientation to use, otherwise use the default.
    int32_t lumpIdx;

    if (frame.rotate) {
        const angle_t angToThing = R_PointToAngle2(gViewX, gViewY, thingX, thingY);
        const uint32_t dirIdx = (angToThing - thingAngle + (ANG45 / 2) * 9) >> 29;      // Note: same calculation as PC Doom

        lumpIdx = frame.lump[dirIdx];
        bFlipSrite = frame.flip[dirIdx];
    } else {
        lumpIdx = frame.lump[0];
        bFlipSrite = frame.flip[0];
    }

    // Upload the sprite texture to VRAM if not already uploaded and return the texture to use
    texture_t& tex = R_GetTexForLump(lumpIdx);
    I_CacheTex(tex);
    return tex;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Populates a sprite fragment entry (covering the entire sprite) for the given thing and using the specified sector color
//------------------------------------------------------------------------------------------------------------------------------------------
static void RV_InitSpriteFrag(
    mobj_t& thing,
    SpriteFrag& sprFrag,
    const fixed_t thingX,
    const fixed_t thingY,
    const fixed_t thingZ,
    const uint8_t secR,
    const uint8_t secG,
    const uint8_t secB
) noexcept {
    // Transform its xyz (Doom xzy) position by the view projection matrix to obtain the depth of the thing.
    // This will be useful later for depth sorting.
    const float thingPos[3] = {
        RV_FixedToFloat(thingX),
        RV_FixedToFloat(thingZ),
        RV_FixedToFloat(thingY)
    };

    float thingDepth;

    {
        float viewXYZ[3];
        gViewProjMatrix.transform3d(thingPos, viewXYZ);
        thingDepth = viewXYZ[2];
    }

    // Grab the sprite frame to use
    const spritedef_t& spriteDef = gSprites[thing.sprite];
    const spriteframe_t& frame = spriteDef.spriteframes[thing.frame & FF_FRAMEMASK];

    // Make sure the sprite is resident in VRAM and get whether it is flipped
    bool bFlipSprite = {};
    const texture_t& tex = RV_CacheThingSpriteFrame(thingX, thingY, thing.angle, frame, bFlipSprite);

    // Get the texture window params for the sprite
    uint16_t texWinX;
    uint16_t texWinY;
    uint16_t texWinW;
    uint16_t texWinH;
    RV_GetTexWinXyWh(tex, texWinX, texWinY, texWinW, texWinH);

    // Determine sprite render alpha and the pipeline/blend-mode to use.
    // Note that a semi-transparency multiply of '128' means 100% strength (no change), or '1.0'.
    uint8_t stMulR = 128;
    uint8_t stMulG = 128;
    uint8_t stMulB = 128;
    uint8_t stMulA = 128;
    VPipelineType drawPipeline = VPipelineType::World_SpriteMasked;

    if (thing.flags & MF_BLEND_MODE_BIT1) {
        drawPipeline = VPipelineType::World_SpriteAdditive;

        if (thing.flags & MF_BLEND_MODE_BIT2) {
            // Additive blend with 25% opacity
            stMulR = 32;
            stMulG = 32;
            stMulB = 32;
        } else {
            // Additive blend with 100% opacity ...
        }
    }
    else if (thing.flags & MF_BLEND_MODE_BIT2) {
        // Subtractive blend with 100% opacity
        drawPipeline = VPipelineType::World_SpriteSubtractive;
    }
    else if (thing.flags & MF_BLEND_ON) {
        // Alpha blend with 50% opacity
        drawPipeline = VPipelineType::World_SpriteAlpha;
        stMulA = 64;
    }

    // Aspect correction scaling value copied from 'R_DrawSubsectorSprites'.
    // See the comments there for more about this...
    constexpr float ASPECT_CORRECT = 4.0f / 5.0f;

    // Get the width and height to draw the sprite with and the offsetting to use
    const float spriteW = (float) tex.width * ASPECT_CORRECT;
    const float spriteH = (float) tex.height;
    const float offsetY = -(float) tex.height + (float) tex.offsetY;
    const float offsetX = bFlipSprite ? 
        (-(float) tex.width + (float) tex.offsetX) * ASPECT_CORRECT :
        (-(float) tex.offsetX) * ASPECT_CORRECT;

    // Get the x axis vector for the view rotation matrix: this will be used to construct the sprite billboard
    float axisX[4];
    gSpriteBillboardMatrix.getRow(0, axisX);

    // Compute the world space xyz position of the sprite (bottom left corner)
    const float worldPos[3] = {
        thingPos[0] + offsetX * axisX[0],
        thingPos[1] + offsetY,
        thingPos[2] + offsetX * axisX[2]
    };

    // Compute the worldspace xz coords for the endpoints and top/bottom of the sprite
    const float p1[2] = {
        worldPos[0],
        worldPos[2]
    };

    const float p2[2] = {
        worldPos[0] + spriteW * axisX[0],
        worldPos[2] + spriteW * axisX[2]
    };

    const float yb = { worldPos[1] };
    const float yt = { worldPos[1] + spriteH };

    // Compute the UV coords for the sprite
    float ul = 0.0f;
    float ur = tex.width;
    const float vt = 0.0f;
    const float vb = spriteH;

    if (bFlipSprite) {
        std::swap(ul, ur);
    }

    // Decide what color to shade the sprite with: some sprites are shaded at 125% intensity (fireballs etc.)
    uint8_t sprColR, sprColG, sprColB;

    if (thing.frame & FF_FULLBRIGHT) {
        sprColR = LIGHT_INTENSTIY_MAX;
        sprColG = LIGHT_INTENSTIY_MAX;
        sprColB = LIGHT_INTENSTIY_MAX;
    } else {
        sprColR = secR;
        sprColG = secG;
        sprColB = secB;
    }

    // Finally populate the sprite fragment
    sprFrag.nextSubsecFragIdx = -1;
    sprFrag.depth = thingDepth;
    sprFrag.x1 = p1[0];
    sprFrag.z1 = p1[1];
    sprFrag.x2 = p2[0];
    sprFrag.z2 = p2[1];
    sprFrag.yt = yt;
    sprFrag.yb = yb;
    sprFrag.ul = ul;
    sprFrag.ur = ur;
    sprFrag.vt = vt;
    sprFrag.vb = vb;
    sprFrag.drawPipeline = drawPipeline;
    sprFrag.colR = sprColR;
    sprFrag.colG = sprColG;
    sprFrag.colB = sprColB;
    sprFrag.stMulR = stMulR;
    sprFrag.stMulG = stMulG;
    sprFrag.stMulB = stMulB;
    sprFrag.stMulA = stMulA;
    sprFrag.texWinX = texWinX;
    sprFrag.texWinY = texWinY;
    sprFrag.texWinW = texWinW;
    sprFrag.texWinH = texWinH;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws the given sprite fragment
//------------------------------------------------------------------------------------------------------------------------------------------
static void RV_DrawSpriteFrag(const SpriteFrag& sprFrag) noexcept {
    VDrawing::setDrawPipeline(sprFrag.drawPipeline);

    const uint8_t colR = sprFrag.colR;
    const uint8_t colG = sprFrag.colG;
    const uint8_t colB = sprFrag.colB;

    VDrawing::addWorldQuad(
        { sprFrag.x1, sprFrag.yb, sprFrag.z1, sprFrag.ul, sprFrag.vb, colR, colG, colB },
        { sprFrag.x1, sprFrag.yt, sprFrag.z1, sprFrag.ul, sprFrag.vt, colR, colG, colB },
        { sprFrag.x2, sprFrag.yt, sprFrag.z2, sprFrag.ur, sprFrag.vt, colR, colG, colB },
        { sprFrag.x2, sprFrag.yb, sprFrag.z2, sprFrag.ur, sprFrag.vb, colR, colG, colB },
        gClutX, gClutY,
        sprFrag.texWinX, sprFrag.texWinY,
        sprFrag.texWinW, sprFrag.texWinH,
        VLightDimMode::None,
        sprFrag.stMulR, sprFrag.stMulG, sprFrag.stMulB, sprFrag.stMulA
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Makes the sprite fragment visit the specified subsector.
// Adds it to the draw list of sprite fragments for that subsector.
//------------------------------------------------------------------------------------------------------------------------------------------
static void RV_SpriteFrag_VisitSubsector(const subsector_t& subsec, const SpriteFrag& frag) noexcept {
    // If the subsector is not drawn then ignore and don't assign the sprite to a draw list
    const int32_t drawSubsecIdx = subsec.vkDrawSubsecIdx;

    if (drawSubsecIdx < 0)
        return;

    ASSERT((size_t) drawSubsecIdx < gRvDrawSubsecSprFrags.size());

    // Add this sprite fragment to the draw list for the subsector
    const int32_t sprFragIdx = (int32_t) gRvSpriteFrags.size();
    SpriteFrag& drawFrag = gRvSpriteFrags.emplace_back(frag);
    drawFrag.nextSubsecFragIdx = gRvDrawSubsecSprFrags[drawSubsecIdx];
    gRvDrawSubsecSprFrags[drawSubsecIdx] = sprFragIdx;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Used to check if a sprite split test line collides against the specified seg.
// Checks to see if the seg is considered blocking for the purposes of sprite splitting, and whether the line crosses it.
// Returns 'true' if a sprite split is allowed to occur after testing against this seg.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool RV_SpriteSplitTest_VisitSeg(const rvseg_t& seg, const SplitTestLine& splitLine) noexcept {
    // If the split line does not intersect the seg's line then the seg does not prevent splitting
    line_t& line = *seg.linedef;
    const float lineV1x = RV_FixedToFloat(line.vertex1->x);
    const float lineV1y = RV_FixedToFloat(line.vertex1->y);
    const float lineV2x = RV_FixedToFloat(line.vertex2->x);
    const float lineV2y = RV_FixedToFloat(line.vertex2->y);

    if (!RV_LinesIntersect(lineV1x, lineV1y, lineV2x, lineV2y, splitLine.x1, splitLine.z1, splitLine.x2, splitLine.z2))
        return true;

    // Otherwise if it is a one-sided line then it prevents splitting in all cases
    sector_t* const pBackSector = seg.backsector;

    if (!pBackSector)
        return false;

    // If it's a two-sided line that is masked or translucent then do not allow sprite splitting across it.
    // Don't want sprites poking through mid wall textures like bars and so on.
    if (line.flags & (ML_MIDMASKED | ML_MIDTRANSLUCENT))
        return false;

    // If the seg is not visible then allow splitting across two sided lines to prevent sprite ordering problems.
    // Since we hopefully won't see the results anyway this should be the best approach to prevent the most ordering problems:
    if ((seg.flags & SGF_VISIBLE_COLS) == 0)
        return true;

    // Treat the 2 sided seg as blocking if the split line would hit either the upper or lower walls.
    // In all other cases allow a split to take place across this seg, even if the split test line crosses it.
    const sector_t& frontSector = *seg.frontsector;

    const float midBy = RV_FixedToFloat(std::max(frontSector.floorDrawH, pBackSector->floorDrawH));
    const float midTy = RV_FixedToFloat(std::min(frontSector.ceilingDrawH, pBackSector->ceilingDrawH));

    return ((splitLine.y >= midBy) && (splitLine.y <= midTy));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tests if a sprite splitting line collides against the specified subsector.
// Performs raycasts between the split line endpoints to see if they cross blocking segs.
// Returns 'true' if a sprite split is allowed.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool RV_SpriteSplitTest_VisitSubsector(const subsector_t& subsec, const SplitTestLine& splitLine) noexcept {
    const int32_t numSegs = subsec.numsegs;
    const rvseg_t* const pSegs = gpRvSegs.get() + subsec.firstseg;

    for (int32_t i = 0; i < numSegs; ++i) {
        if (!RV_SpriteSplitTest_VisitSeg(pSegs[i], splitLine))
            return false;
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Used to test if a sprite splitting line collides against the specified BSP tree node and it's children.
// Tests to make sure the line doesn't cross any lines we don't want to split against, like one sided walls.
// Returns 'true' if a sprite split is allowed.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool RV_SpriteSplitTest_VisitBspNode(const int32_t nodeIdx, const SplitTestLine& splitLine) noexcept {
    // Is this node number a subsector?
    if (nodeIdx & NF_SUBSECTOR) {
        // Note: this strange check is in the PC engine too...
        // Under what circumstances can the node number be '-1'?
        if (nodeIdx == -1) {
            return RV_SpriteSplitTest_VisitSubsector(gpSubsectors[0], splitLine);
        } else {
            return RV_SpriteSplitTest_VisitSubsector(gpSubsectors[nodeIdx & (~NF_SUBSECTOR)], splitLine);
        }
    } else {
        // This is not a subsector, continue traversing the BSP tree and testing against it
        node_t& node = gpBspNodes[nodeIdx];

        // Compute which side of the split the line endpoints are on using the cross product.
        // This is pretty much the same code found in 'R_PointOnSide':
        const float nodePx = RV_FixedToFloat(node.line.x);
        const float nodePy = RV_FixedToFloat(node.line.y);
        const float nodeDx = RV_FixedToFloat(node.line.dx);
        const float nodeDy = RV_FixedToFloat(node.line.dy);

        const float relX1 = splitLine.x1 - nodePx;
        const float relX2 = splitLine.x2 - nodePx;
        const float relZ1 = splitLine.z1 - nodePy;
        const float relZ2 = splitLine.z2 - nodePy;

        const float lprod1 = nodeDx * relZ1;
        const float rprod1 = nodeDy * relX1;
        const float lprod2 = nodeDx * relZ2;
        const float rprod2 = nodeDy * relX2;

        const int32_t side1 = (lprod1 < rprod1) ? 0 : 1;
        const int32_t side2 = (lprod2 < rprod2) ? 0 : 1;

        // Test against the sides of the tree that the line endpoints are on
        if (!RV_SpriteSplitTest_VisitBspNode(node.children[side1], splitLine))
            return false;

        if (side1 != side2) {
            if (!RV_SpriteSplitTest_VisitBspNode(node.children[side2], splitLine))
                return false;
        }
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Does recursive traversal of the BSP tree against the specified sprite fragment.
// Splits up the fragment along BSP split boundaries as needed and assigns the fragments to appropriate destination subsectors.
//------------------------------------------------------------------------------------------------------------------------------------------
static void RV_SpriteFrag_VisitBspNode(const int32_t nodeIdx, SpriteFrag& frag) noexcept {
    // Is this node number a subsector? If so then add the sprite fragment to it's draw lists
    if (nodeIdx & NF_SUBSECTOR) {
        // Note: this strange check is in the PC engine too...
        // Under what circumstances can the node number be '-1'?
        if (nodeIdx == -1) {
            RV_SpriteFrag_VisitSubsector(gpSubsectors[0], frag);
        } else {
            RV_SpriteFrag_VisitSubsector(gpSubsectors[nodeIdx & (~NF_SUBSECTOR)], frag);
        }
    } else {
        // This is not a subsector, continue traversing the BSP tree and splitting the sprite fragment
        node_t& node = gpBspNodes[nodeIdx];

        // Compute which side of the split the sprite endpoints are on using the cross product.
        // This is pretty much the same code found in 'R_PointOnSide':
        const float nodePx = RV_FixedToFloat(node.line.x);
        const float nodePy = RV_FixedToFloat(node.line.y);
        const float nodeDx = RV_FixedToFloat(node.line.dx);
        const float nodeDy = RV_FixedToFloat(node.line.dy);

        const float relX1 = frag.x1 - nodePx;
        const float relX2 = frag.x2 - nodePx;
        const float relZ1 = frag.z1 - nodePy;
        const float relZ2 = frag.z2 - nodePy;

        const float lprod1 = nodeDx * relZ1;
        const float rprod1 = nodeDy * relX1;
        const float lprod2 = nodeDx * relZ2;
        const float rprod2 = nodeDy * relX2;

        const bool bSide1 = (lprod1 < rprod1);
        const bool bSide2 = (lprod2 < rprod2);

        // Do we need to do a split or not?
        if (bSide1 == bSide2) {
            // No split needed, just recurse into the appropriate side
            if (bSide1) {
                RV_SpriteFrag_VisitBspNode(node.children[0], frag);
            } else {
                RV_SpriteFrag_VisitBspNode(node.children[1], frag);
            }
        } else {
            // Need to split (less common case): need to compute where the split would happen.
            // First get the un-normalized normal vector for the node.
            const float nodeNx = -nodeDy;
            const float nodeNy = +nodeDx;

            // Compute the scaled perpendicular distance of each billboard/sprite-fragment point to the node plane
            const float dist1 = std::abs(relX1 * nodeNx + relZ1 * nodeNy);
            const float dist2 = std::abs(relX2 * nodeNx + relZ2 * nodeNy);

            // Compute the 'time' of the intersection/split
            const float splitT = std::clamp(dist1 / (dist1 + dist2), 0.0f, 1.0f);

            // Compute points a little bit on either side of the split.
            // We will make a line between these points and intersect against geometry to see if the split line can be crossed.
            // In some cases we want to prohibit splits if the split would occur against a 1 sided line, for example.
            //
            // I wish there was a better way to do this, but unfortunately the BSP tree doesn't contain any info about what lines originally
            // generated the splits, so we have no idea or way of telling whether a node line is solid/blocking or not.
            // This collision test is a workaround of sorts.
            const float splitT1 = std::clamp(splitT * 0.99f, 0.0f, 1.0f);
            const float splitT2 = std::clamp(splitT * 1.01f, 0.0f, 1.0f);
            const float splitT1_inv = (1.0f - splitT1);
            const float splitT2_inv = (1.0f - splitT2);

            const SplitTestLine splitLine = {
                frag.x1 * splitT1_inv + frag.x2 * splitT1,
                frag.z1 * splitT1_inv + frag.z2 * splitT1,
                frag.x1 * splitT2_inv + frag.x2 * splitT2,
                frag.z1 * splitT2_inv + frag.z2 * splitT2,
                (frag.yt + frag.yb) * 0.5f  // Note: make the test line be at 1/2 of the height of the sprite; splits can happen across small step ups, not large ones
            };

            // Test the split line against geometry to see if this split would be allowed
            if (!RV_SpriteSplitTest_VisitBspNode(nodeIdx, splitLine)) {
                // Can't split, decide which part of the tree to place the sprite fragment in based on the sprite's center point.
                // If splits are not possible then ultimately we will tend to put the thing's sprite parts closest to it's home subsector for rendering.
                const float lprod_center = nodeDx * (gSpriteFragThingPos[2] - nodePy);
                const float rprod_center = nodeDy * (gSpriteFragThingPos[0] - nodePx);
                const bool bCenterSide = (lprod_center < rprod_center);

                if (bCenterSide) {
                    RV_SpriteFrag_VisitBspNode(node.children[0], frag);
                } else {
                    RV_SpriteFrag_VisitBspNode(node.children[1], frag);
                }
            }
            else {
                // Can split! Create two new smaller fragments, split along the split point.
                const float splitT_inv = 1.0f - splitT;

                SpriteFrag frag2 = frag;
                frag2.x1 = frag.x1 * splitT_inv + frag.x2 * splitT;
                frag2.z1 = frag.z1 * splitT_inv + frag.z2 * splitT;
                frag2.ul = frag.ul * splitT_inv + frag.ur * splitT;

                SpriteFrag frag1 = frag;
                frag1.x2 = frag2.x1;
                frag1.z2 = frag2.z1;
                frag1.ur = frag2.ul;

                // Recurse using the split fragments.
                // Splits shouldn't happen TOO often so hopefully stack space should not be an issue.
                if (bSide1) {
                    RV_SpriteFrag_VisitBspNode(node.children[0], frag1);
                    RV_SpriteFrag_VisitBspNode(node.children[1], frag2);
                } else {
                    RV_SpriteFrag_VisitBspNode(node.children[1], frag1);
                    RV_SpriteFrag_VisitBspNode(node.children[0], frag2);
                }
            }
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Creates all of the sprite fragments for sprites contained in the specfied subsector
//------------------------------------------------------------------------------------------------------------------------------------------
static void RV_BuildSubsectorSpriteFrags(const subsector_t& subsec, [[maybe_unused]] const int32_t drawSubsecIdx) noexcept {
    // Sanity check!
    ASSERT((size_t) drawSubsecIdx < gRvDrawSubsecs.size());

    // Early out if there are no things in the sector
    sector_t& sector = *subsec.sector;

    if (!sector.thinglist)
        return;

    // Build all fragments for this subsector
    for (mobj_t* pThing = sector.thinglist; pThing; pThing = pThing->snext) {
        // Ignore the thing if not in this subsector
        if (pThing->subsector != &subsec)
            continue;

        // Ignore the thing if in state 'S_NULL' (needed to avoid rendering PsyDoom's new 'marker' things)
        if (pThing->state == &gStates[S_NULL])
            continue;

        // Ignore this thing if we're viewing from it and we are not using the external camera
        if ((gpViewPlayer->mo == pThing) && (gExtCameraTicsLeft <= 0))
            continue;

        // Get the interpolated position of the sprite
        const fixed_t thingX = pThing->x.renderValue();
        const fixed_t thingY = pThing->y.renderValue();
        const fixed_t thingZ = pThing->z.renderValue();

        // Get the light/color value for the thing at it's z-height
        uint8_t secR;
        uint8_t secG;
        uint8_t secB;
        R_GetSectorDrawColor(*subsec.sector, thingZ, secR, secG, secB);

        // Allocate and initialize a full sprite fragment for the thing
        SpriteFrag sprFrag;
        RV_InitSpriteFrag(*pThing, sprFrag, thingX, thingY, thingZ, secR, secG, secB);

        // Split up the sprite fragment into further small pieces (on subsector boundaries) if neccessary and remember the position of the thing being split.
        // The thing position is used to resolve cases that we can't split and where we need to decide on a sprite subsector.
        gSpriteFragThingPos[0] = RV_FixedToFloat(thingX);
        gSpriteFragThingPos[1] = RV_FixedToFloat(thingZ);   // N.B: converting to Vulkan coords (where 'y' is up and 'z' is foward)
        gSpriteFragThingPos[2] = RV_FixedToFloat(thingY);

        const int32_t bspRootNodeIdx = gNumBspNodes - 1;
        RV_SpriteFrag_VisitBspNode(bspRootNodeIdx, sprFrag);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Builds a list of all the sprite fragments to be drawn for this frame
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_BuildSpriteFragLists() noexcept {
    // Clear the list of sprite fragments to draw and init each draw subsector as having no sprite frags.
    // ALso prealloc a minimum amount of memory for all of the draw vectors.
    const int32_t numDrawSubsecs = (int32_t) gRvDrawSubsecs.size();

    gRvSpriteFrags.clear();
    gRvSpriteFrags.reserve(8192);
    gRvDrawSubsecSprFrags.clear();
    gRvDrawSubsecSprFrags.reserve(4196);
    gRvDrawSubsecSprFrags.resize((size_t) numDrawSubsecs, -1);
    gRvSortedFrags.reserve(256);

    // Run through all of the draw subsectors and build a list of sprite fragments for each
    for (int32_t drawSubsecIdx = 0; drawSubsecIdx < numDrawSubsecs; ++drawSubsecIdx) {
        RV_BuildSubsectorSpriteFrags(*gRvDrawSubsecs[drawSubsecIdx], drawSubsecIdx);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draw sprite fragments for the specified draw subsector index
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_DrawSubsecSpriteFrags(const int32_t drawSubsecIdx) noexcept {
    // Firstly gather all of the sprite fragments for this draw subsector
    ASSERT(gRvSortedFrags.empty());
    ASSERT((size_t) drawSubsecIdx < gRvDrawSubsecs.size());

    int32_t nextSprIdx = gRvDrawSubsecSprFrags[drawSubsecIdx];
    const SpriteFrag* const pAllSprFrags = gRvSpriteFrags.data();

    while (nextSprIdx >= 0) {
        ASSERT((size_t) nextSprIdx < gRvSpriteFrags.size());
        const SpriteFrag& sprFrag = pAllSprFrags[nextSprIdx];
        gRvSortedFrags.emplace_back(&sprFrag);
        nextSprIdx = sprFrag.nextSubsecFragIdx;
    }

    // Sort all of the sprite fragments back to front
    std::stable_sort(
        gRvSortedFrags.begin(),
        gRvSortedFrags.end(),
        [](const SpriteFrag* const pFrag1, const SpriteFrag* const pFrag2) noexcept {
            return (pFrag1->depth > pFrag2->depth);
        }
    );

    // Draw all the sorted fragments and clear the temporary list to finish up
    for (const SpriteFrag* const pSprFrag : gRvSortedFrags) {
        RV_DrawSpriteFrag(*pSprFrag);
    }

    gRvSortedFrags.clear();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws the player weapon for the Vulkan renderer.
// Implemented natively for Vulkan so that weapon sway can be interpolated to a higher precision.
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_DrawWeapon() noexcept {
    // Run through all of the player sprites for the view player and render
    player_t& player = *gpViewPlayer;
    pspdef_t* pSprite = player.psprites;

    for (int32_t pspIdx = 0; pspIdx < NUMPSPRITES; ++pspIdx, ++pSprite) {
        // Is this particular player sprite slot showing anything?
        if (!pSprite->state)
            continue;

        // Get the texture for the sprite and upload to VRAM if required
        const state_t& state = *pSprite->state;
        const spritedef_t& spriteDef = gSprites[state.sprite];
        const int32_t frameNum = state.frame & FF_FRAMEMASK;
        const spriteframe_t& frame = spriteDef.spriteframes[frameNum];

        texture_t& tex = R_GetTexForLump(frame.lump[0]);
        I_CacheTex(tex);

        // Set the blending mode to use (alpha or additive)
        const bool bIsTransparent = ((player.mo->flags & MF_ALL_BLEND_FLAGS) != 0);

        if (bIsTransparent) {
            VDrawing::setDrawPipeline(VPipelineType::UI_8bpp_Add);
        } else {
            VDrawing::setDrawPipeline(VPipelineType::UI_8bpp);
        }
        
        // Get the size and location of the weapon sprite
        const float sprX = RV_FixedToFloat(pSprite->sx.renderValue()) + (float) HALF_SCREEN_W - (float) tex.offsetX;
        const float sprY = RV_FixedToFloat(pSprite->sy.renderValue()) + (float) VIEW_3D_H - 1.0f - (float) tex.offsetY;
        const float sprW = tex.width;
        const float sprH = tex.height;

        // Figure out the color of the weapon sprite from the player's sector
        mobj_t& playerMobj = *player.mo;
        const sector_t& sector = *playerMobj.subsector->sector;
        uint8_t sprR, sprG, sprB = {};

        if (state.frame & FF_FULLBRIGHT) {
            // Note: these magic 5/8 multipliers correspond VERY closely to 'LIGHT_INTENSTIY_MAX / 255'.
            // The resulting values are sometimes not quite the same however.
            const light_t light = R_GetSectorLightColor(sector, playerMobj.z.renderValue());
            sprR = (uint8_t)(((uint32_t) light.r * 5) / 8);
            sprG = (uint8_t)(((uint32_t) light.g * 5) / 8);
            sprB = (uint8_t)(((uint32_t) light.b * 5) / 8);
        }
        else {
            R_GetSectorDrawColor(sector, playerMobj.z.renderValue(), sprR, sprG, sprB);
        }

        // Get CLUT and texture window information
        uint16_t clutX, clutY = {};
        uint16_t texWinX, texWinY = {};
        uint16_t texWinW, texWinH = {};

        RV_ClutIdToClutXy(g3dViewPaletteClutId, clutX, clutY);
        RV_GetTexWinXyWh(tex, texWinX, texWinY, texWinW, texWinH);

        // Draw the sprite and restrict the texture window to cover the exact area of VRAM occupied by the sprite.
        // Ignoring the gpu texture window/page settings in this way and restricting to the exact pixels used by the
        // sprite helps to avoid stitching artifacts, especially when MSAA is active.
        VDrawing::addUISprite(
            sprX,
            sprY,
            sprW,
            sprH,
            0,          // UV coords are local to the texture window, which covers the entire sprite area
            0,
            sprR,
            sprG,
            sprB,
            128,
            clutX,
            clutY,
            texWinX,
            texWinY,
            texWinW,
            texWinH
        );
    }
}

#endif  // #if PSYDOOM_VULKAN_RENDERER
