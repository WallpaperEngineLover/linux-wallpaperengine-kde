#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Render/Objects/PuppetIK.h"

using Catch::Approx;
using namespace WallpaperEngine::Render::Objects;

namespace {
glm::mat4 at (float x, float y, float z) { return glm::translate (glm::mat4 (1.0f), glm::vec3 (x, y, z)); }

glm::vec3 where (const glm::mat4& matrix) { return glm::vec3 (matrix[3]); }

float apart (const glm::mat4& a, const glm::mat4& b) { return glm::length (where (a) - where (b)); }

struct Arm {
    Arm () : model { at (0, 0, 0), at (1, 0, 0), at (2, 0, 0) }, bind (model), extras { at (0, 0, 0) }, joints (3) {
	rig.lengths = { 0.0f, 1.0f, 1.0f };
	rig.childDirections = { { { 1, glm::vec3 (1, 0, 0) } }, { { 2, glm::vec3 (1, 0, 0) } }, {} };
	rig.targets = { { 2, 0 } };

	PuppetIKEntry entry;

	entry.endBone = 2;
	entry.flags = 1;
	entry.length = 2.0f;
	entry.bones = { 0, 1, 2 };
	chain.root = 0;
	chain.links = { { .bone = 0, .entries = { entry } } };
	ik.resize (3);
    }

    void solve () { solvePuppetIKChain (rig, chain, parents, ik, model, extras, bind, glm::mat4 (1.0f), joints); }

    PuppetIKRig rig;
    PuppetIKChain chain;
    std::vector<int> parents = { 0, 0, 1 };
    std::vector<PuppetBoneIK> ik;
    std::vector<glm::mat4> model;
    std::vector<glm::mat4> bind;
    std::vector<glm::mat4> extras;
    std::vector<PuppetRopeJoint> joints;
};
}

TEST_CASE ("A bone's IK settings are read from its JSON") {
    const auto none = PuppetBoneIK::parse ("not json");

    CHECK (none.flags == 0);
    CHECK (PuppetBoneIK::parse ("[1]").flags == 0);
    CHECK (PuppetBoneIK::parse (R"({"ik":false,"ge":true})").flags == 0);

    const auto full = PuppetBoneIK::parse (
	R"({"ik":true,"ge":true,"m":5,"tf":50,"ikd":0,"ikg":true,"ikr":true,"ikrd":0.5,"ikse":true,"ikfe":true})"
    );

    CHECK (
	full.flags
	== (PuppetBoneIK::Enabled | PuppetBoneIK::Gravity | PuppetBoneIK::Grounded | PuppetBoneIK::RotateToTarget
	    | PuppetBoneIK::SolveEnd | PuppetBoneIK::FollowParent)
    );
    CHECK (full.mass == Approx (5.0f));
    CHECK (full.friction == Approx (0.25f));
    CHECK (full.depth == 1);
    CHECK (full.rotateDistance == Approx (1.0f));

    const auto plain = PuppetBoneIK::parse (R"({"ik":true})");

    CHECK (plain.flags == PuppetBoneIK::Enabled);
    CHECK (plain.depth == 2);
    CHECK (plain.mass == Approx (1000.0f));
    CHECK (plain.rotateDistance == Approx (50.0f));

    CHECK (PuppetBoneIK::parse (R"({"ik":true,"tf":300})").friction == Approx (1.0f));
    CHECK (PuppetBoneIK::parse (R"({"ik":true,"ikd":6,"ikrd":80,"ikr":true})").depth == 6);
    CHECK (PuppetBoneIK::parse (R"({"ik":true,"ikd":6,"ikrd":80,"ikr":true})").rotateDistance == Approx (80.0f));

    CHECK (PuppetBoneIK::parse (R"({"ik":true,"se":true,"t":true,"tf":50})").friction == Approx (50.0f));
    CHECK (PuppetBoneIK::parse (R"({"ik":true,"re":true,"r":true,"tf":50})").friction == Approx (50.0f));
}

TEST_CASE ("Constrained joints carry their angle limits unless a simulation drives them") {
    const auto limited = PuppetBoneIK::parse (R"({"ikce":true,"ikrminl":-0.5,"ikrmaxl":0.75})");

    CHECK (limited.flags == PuppetBoneIK::Constrained);
    CHECK (limited.minAngle == Approx (-0.5f));
    CHECK (limited.maxAngle == Approx (0.75f));

    const auto simulated = PuppetBoneIK::parse (R"({"ikce":true,"se":true,"r":true,"ikrminl":-0.5,"ikrmaxl":0.75})");

    CHECK (simulated.flags == PuppetBoneIK::Constrained);
    CHECK (simulated.minAngle == 0.0f);
    CHECK (simulated.maxAngle == 0.0f);

    const auto both = PuppetBoneIK::parse (R"({"ik":true,"ikce":true,"ikrminl":-0.5})");

    CHECK (both.flags == (PuppetBoneIK::Constrained | PuppetBoneIK::Enabled));
    CHECK (both.minAngle == 0.0f);
}

TEST_CASE ("Blending two transforms moves rotation, scale and position by the weight") {
    const glm::mat4 from = glm::rotate (at (0, 0, 0), 0.0f, glm::vec3 (0, 0, 1));
    const glm::mat4 to = glm::scale (glm::rotate (at (4, 2, 0), 1.0f, glm::vec3 (0, 0, 1)), glm::vec3 (3.0f));

    const auto start = blendPuppetTransform (from, to, 0.0f);
    const auto end = blendPuppetTransform (from, to, 1.0f);
    const auto half = blendPuppetTransform (from, to, 0.5f);

    for (int column = 0; column < 4; column++) {
	for (int row = 0; row < 4; row++) {
	    CAPTURE (column, row);
	    CHECK (start[column][row] == Approx (from[column][row]).margin (1e-5));
	    CHECK (end[column][row] == Approx (to[column][row]).margin (1e-4));
	}
    }

    CHECK (glm::length (where (half) - glm::vec3 (2, 1, 0)) < 1e-5);
    CHECK (glm::length (glm::vec3 (half[0])) == Approx (2.0f).margin (1e-4));
    CHECK (std::atan2 (half[0][1], half[0][0]) == Approx (0.5f).margin (1e-4));
}

TEST_CASE ("Wind blows only where the scene has it and never against its direction") {
    PuppetRopeEnvironment environment;
    glm::vec3 force (9.0f);

    CHECK_FALSE (environment.windAt (glm::vec3 (0.0f), force));
    CHECK (force == glm::vec3 (9.0f));

    environment.wind = true;
    environment.windDirection = glm::vec3 (1.0f, 0.0f, 0.0f);
    environment.windStrength = 4.0f;

    CHECK (environment.windAt (glm::vec3 (1.0f), force));
    CHECK (force == glm::vec3 (0.0f));

    environment.width = 100.0f;

    bool blew = false;

    for (int i = 0; i < 200; i++) {
	environment.clock = static_cast<float> (i) * 0.37f;

	CHECK (environment.windAt (glm::vec3 (static_cast<float> (i), 0, 0), force));
	CHECK (force.x >= 0.0f);
	CHECK (force.x <= 4.0f);
	CHECK (force.y == 0.0f);
	CHECK (force.z == 0.0f);
	blew = blew || force.x > 0.1f;
    }

    CHECK (blew);
}

TEST_CASE ("A constrained joint is turned back inside its limits") {
    PuppetBoneIK settings;

    settings.minAngle = -0.5f;
    settings.maxAngle = 0.5f;

    const glm::vec3 parent (0, 0, 0);
    const glm::vec3 joint (1, 0, 0);

    SECTION ("inside the range nothing changes") {
	glm::vec3 direction (std::cos (0.2f), std::sin (0.2f), 0.0f);

	limitPuppetJointDirection (settings, joint, parent, direction);
	CHECK (direction.x == Approx (std::cos (0.2f)));
	CHECK (direction.y == Approx (std::sin (0.2f)));
    }

    SECTION ("past the upper limit it stops at it") {
	glm::vec3 direction (std::cos (1.0f), std::sin (1.0f), 0.0f);

	limitPuppetJointDirection (settings, joint, parent, direction);
	CHECK (direction.x == Approx (std::cos (0.5f)).margin (1e-5));
	CHECK (direction.y == Approx (std::sin (0.5f)).margin (1e-5));
    }

    SECTION ("past the lower limit it stops at it") {
	glm::vec3 direction (std::cos (-1.2f), std::sin (-1.2f), 0.0f);

	limitPuppetJointDirection (settings, joint, parent, direction);
	CHECK (direction.x == Approx (std::cos (-0.5f)).margin (1e-5));
	CHECK (direction.y == Approx (std::sin (-0.5f)).margin (1e-5));
    }
}

TEST_CASE ("A chain solved for a target it can reach ends on it with its bones the same length") {
    Arm arm;

    arm.extras[0] = at (1.0f, 1.0f, 0.0f);
    arm.solve ();

    CHECK (glm::length (where (arm.model[0]) - glm::vec3 (0, 0, 0)) < 1e-6);
    CHECK (apart (arm.model[1], arm.model[0]) == Approx (1.0f).margin (1e-4));
    CHECK (apart (arm.model[2], arm.model[1]) == Approx (1.0f).margin (1e-4));
    CHECK (apart (arm.model[2], arm.extras[0]) < 0.32f);
}

TEST_CASE ("A chain asked for a target out of reach stretches straight towards it") {
    Arm arm;

    arm.extras[0] = at (0.0f, 5.0f, 0.0f);
    arm.solve ();

    CHECK (glm::length (where (arm.model[1]) - glm::vec3 (0, 1, 0)) < 1e-4);
    CHECK (glm::length (where (arm.model[2]) - glm::vec3 (0, 2, 0)) < 1e-4);

    const glm::vec3 row = glm::vec3 (arm.model[0][0]);

    CHECK (glm::length (row) == Approx (1.0f).margin (1e-4));
    CHECK (std::fabs (row.x) < 1e-4f);
}

TEST_CASE ("A chain whose target is straight ahead is not rotated at all") {
    Arm arm;

    arm.extras[0] = at (5.0f, 0.0f, 0.0f);
    arm.solve ();

    CHECK (glm::length (where (arm.model[2]) - glm::vec3 (2, 0, 0)) < 1e-4);
    CHECK (glm::length (glm::vec3 (arm.model[0][0]) - glm::vec3 (1, 0, 0)) < 1e-5);
    CHECK (glm::length (glm::vec3 (arm.model[0][1]) - glm::vec3 (0, 1, 0)) < 1e-5);
    CHECK (glm::length (glm::vec3 (arm.model[1][2]) - glm::vec3 (0, 0, 1)) < 1e-5);
}

TEST_CASE ("A chain that is closer than its minimum reach keeps its end that far away") {
    Arm arm;

    arm.chain.links[0].entries[0].minReach = 1.5f;
    arm.extras[0] = at (0.5f, 0.0f, 0.0f);
    arm.model[2] = at (0.5f, 0.0f, 0.0f);
    arm.solve ();

    CHECK (apart (arm.model[2], arm.model[0]) >= 1.49f);
}

TEST_CASE ("The end bone of a chain takes the orientation of its target") {
    Arm arm;

    arm.extras[0] = glm::rotate (at (1.0f, 1.0f, 0.0f), 0.5f, glm::vec3 (0, 0, 1));
    arm.solve ();

    CHECK (std::atan2 (arm.model[2][0][1], arm.model[2][0][0]) == Approx (0.5f).margin (1e-2));

    SECTION ("a follower end copies the bone before it instead") {
	Arm follower;

	follower.ik[2].flags = PuppetBoneIK::FollowParent;
	follower.extras[0] = at (1.0f, 1.0f, 0.0f);
	follower.solve ();

	CHECK (glm::length (glm::vec3 (follower.model[2][0]) - glm::vec3 (follower.model[1][0])) < 1e-6);
    }

    SECTION ("a rotating end turns towards a target it misses") {
	Arm rotating;

	rotating.ik[2].flags = PuppetBoneIK::RotateToTarget;
	rotating.ik[2].rotateDistance = 1.0f;
	rotating.extras[0] = at (0.0f, 5.0f, 0.0f);
	rotating.solve ();

	const glm::vec3 row = glm::normalize (glm::vec3 (rotating.model[2][0]));

	CHECK (row.y == Approx (1.0f).margin (1e-2));
    }
}

TEST_CASE ("A chain with a bone that does not exist is left alone") {
    Arm arm;

    arm.chain.links[0].entries[0].bones = { 0, 1, 7 };
    arm.extras[0] = at (0.0f, 5.0f, 0.0f);
    arm.solve ();

    CHECK (arm.model[1] == at (1, 0, 0));
    CHECK (arm.model[2] == at (2, 0, 0));

    Arm noRoot;

    noRoot.chain.root = 9;
    noRoot.extras[0] = at (0.0f, 5.0f, 0.0f);
    noRoot.solve ();
    CHECK (noRoot.model[2] == at (2, 0, 0));
}

TEST_CASE ("A bend towards a pole picks the side of the pole") {
    Arm arm;

    arm.rig.poles = { { 2, 1 } };
    arm.extras.push_back (at (1.0f, 3.0f, 0.0f));
    arm.extras[0] = at (2.0f, 0.0f, 0.0f);
    arm.model[2] = at (2.0f, 0.0f, 0.0f);
    arm.model[1] = at (1.0f, -0.5f, 0.0f);
    arm.chain.links[0].entries[0].length = 2.0f;
    arm.chain.links[0].entries[0].flags = 3;
    arm.solve ();

    CHECK (arm.model[1][3].y > 0.0f);
    CHECK (apart (arm.model[1], arm.model[0]) == Approx (1.0f).margin (1e-3));
}

TEST_CASE ("A joint limit reaches into the solver") {
    Arm arm;

    arm.ik[1].flags = PuppetBoneIK::Constrained;
    arm.ik[1].minAngle = -0.1f;
    arm.ik[1].maxAngle = 0.1f;
    arm.extras[0] = at (0.0f, 1.2f, 0.0f);
    arm.solve ();

    CHECK (apart (arm.model[1], arm.model[0]) == Approx (1.0f).margin (1e-3));
    CHECK (apart (arm.model[2], arm.model[1]) == Approx (1.0f).margin (1e-3));
}

TEST_CASE ("A rope hangs from its root, its end pinned, its middle falling under gravity") {
    Arm arm;
    PuppetRopeEnvironment environment;

    arm.chain.links[0].entries[0].flags = 4;
    arm.ik[2].flags = PuppetBoneIK::Gravity;
    arm.ik[2].mass = 10.0f;

    for (int i = 0; i < 3; i++) {
	stepPuppetRopeChain (
	    arm.rig, arm.chain, arm.ik, arm.model, arm.extras, glm::mat4 (1.0f), arm.joints, environment, 0.1f
	);
    }

    CHECK (arm.joints[0].started);
    CHECK (where (arm.model[0]) == glm::vec3 (0, 0, 0));
    CHECK (glm::length (where (arm.model[2]) - glm::vec3 (2, 0, 0)) < 1e-3f);
    CHECK (arm.model[1][3].y < -0.1f);
    CHECK (arm.model[1][3].x == Approx (1.0f));
}

TEST_CASE ("A rope without gravity or wind stays where it is and one that can't stretch pulls its end in") {
    Arm arm;
    PuppetRopeEnvironment environment;

    arm.chain.links[0].entries[0].flags = 4;

    for (int i = 0; i < 3; i++) {
	stepPuppetRopeChain (
	    arm.rig, arm.chain, arm.ik, arm.model, arm.extras, glm::mat4 (1.0f), arm.joints, environment, 0.1f
	);
    }

    CHECK (glm::length (where (arm.model[1]) - glm::vec3 (1, 0, 0)) < 1e-6);

    Arm taut;

    taut.chain.links[0].entries[0].flags = 5;
    taut.rig.targets = { { 2, 0 } };
    taut.extras[0] = at (0.0f, 6.0f, 0.0f);
    stepPuppetRopeChain (
	taut.rig, taut.chain, taut.ik, taut.model, taut.extras, glm::mat4 (1.0f), taut.joints, environment, 0.1f
    );
    CHECK (glm::length (where (taut.model[2]) - glm::vec3 (0, 2, 0)) < 1e-4);

    Arm stretching;

    stretching.chain.links[0].entries[0].flags = 5;
    stretching.ik[2].flags = PuppetBoneIK::SolveEnd;
    stretching.extras[0] = at (0.0f, 6.0f, 0.0f);
    stepPuppetRopeChain (
	stretching.rig, stretching.chain, stretching.ik, stretching.model, stretching.extras, glm::mat4 (1.0f),
	stretching.joints, environment, 0.1f
    );
    CHECK (glm::length (where (stretching.model[2]) - glm::vec3 (0, 6, 0)) < 1e-4);
}

TEST_CASE ("Ropes are skipped when their bones are missing or they are not ropes") {
    Arm arm;
    PuppetRopeEnvironment environment;

    stepPuppetRopeChain (
	arm.rig, arm.chain, arm.ik, arm.model, arm.extras, glm::mat4 (1.0f), arm.joints, environment, 0.1f
    );
    CHECK_FALSE (arm.joints[0].started);

    arm.chain.links[0].entries[0].flags = 4;
    arm.chain.links[0].entries[0].bones = { 0, 1, 8 };
    stepPuppetRopeChain (
	arm.rig, arm.chain, arm.ik, arm.model, arm.extras, glm::mat4 (1.0f), arm.joints, environment, 0.1f
    );
    CHECK_FALSE (arm.joints[0].started);
}

TEST_CASE ("Extra controllers follow the bone their rules name") {
    Arm arm;

    arm.model[1] = at (4, 0, 0);
    arm.chain.extras = { 0 };
    arm.rig.extraBones = { 2 };
    arm.extras = { at (0, 0, 0) };

    SECTION ("an origin rule moves only the position by the weight") {
	arm.rig.extraRules = { { { .flags = 1, .weight = 0, .target = 1 } } };
	applyPuppetChainConstraints (arm.rig, arm.chain, { 0.25f }, arm.model, arm.extras);
	CHECK (glm::length (where (arm.extras[0]) - glm::vec3 (1, 0, 0)) < 1e-3f);
    }

    SECTION ("a plain rule blends the whole transform") {
	arm.rig.extraRules = { { { .flags = 0, .weight = 0, .target = 1 } } };
	applyPuppetChainConstraints (arm.rig, arm.chain, { 0.5f }, arm.model, arm.extras);
	CHECK (glm::length (where (arm.extras[0]) - glm::vec3 (2, 0, 0)) < 1e-3f);
    }

    SECTION ("rules naming a missing bone or weight change nothing") {
	arm.rig.extraRules = { { { .flags = 1, .weight = 0, .target = 9 }, { .flags = 1, .weight = 5, .target = 1 } } };
	applyPuppetChainConstraints (arm.rig, arm.chain, { 1.0f }, arm.model, arm.extras);
	CHECK (where (arm.extras[0]) == glm::vec3 (0, 0, 0));
    }

    SECTION ("an extra the rig does not know is skipped") {
	arm.chain.extras = { 3 };
	arm.rig.extraRules = { { { .flags = 1, .weight = 0, .target = 1 } } };
	applyPuppetChainConstraints (arm.rig, arm.chain, { 1.0f }, arm.model, arm.extras);
	CHECK (where (arm.extras[0]) == glm::vec3 (0, 0, 0));
    }

    SECTION ("an axis rule keeps the controller on the target's side along its axis") {
	arm.rig.extraRules = { { { .flags = 2, .weight = 0, .target = 1, .start = 0.0f, .range = 1.0f } } };
	arm.extras[0] = at (6, 0, 0);
	applyPuppetChainConstraints (arm.rig, arm.chain, { 1.0f }, arm.model, arm.extras);
	CHECK (std::isfinite (arm.extras[0][3].x));
    }
}
