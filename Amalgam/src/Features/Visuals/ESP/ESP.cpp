#include "ESP.h"

#include "../Groups/Groups.h"
#include "../../Players/PlayerUtils.h"
#include "../../Spectate/Spectate.h"
#include "../../Simulation/MovementSimulation/MovementSimulation.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"

static inline void StorePlayer(CTFPlayer* pPlayer, CTFPlayer* pLocal, Group_t* pGroup, std::unordered_map<CBaseEntity*, PlayerCache_t>& mCache)
{
	int iIndex = pPlayer->entindex();

	if (int iObserverMode = pLocal->m_iObserverMode(); iObserverMode == OBS_MODE_FIRSTPERSON || iObserverMode == OBS_MODE_THIRDPERSON
		? iObserverMode == OBS_MODE_FIRSTPERSON && pLocal->m_hObserverTarget().GetEntryIndex() == iIndex
		: !I::Input->CAM_IsThirdPerson() && iIndex == I::EngineClient->GetLocalPlayer())
		return;

	auto pWeapon = pPlayer->m_hActiveWeapon()->As<CTFWeaponBase>();
	auto pResource = H::Entities.GetResource();
	bool bLocal = pPlayer->entindex() == I::EngineClient->GetLocalPlayer();
	int iClassNum = pPlayer->m_iClass();

	PlayerCache_t& tCache = mCache[pPlayer];
	tCache.m_vText.reserve(8); // typical name/distance/health/class/weapon + a few buffs
	tCache.m_flAlpha = pGroup->m_tColor.a / 255.f;
	tCache.m_tColor = F::Groups.GetColor(pPlayer, pGroup).Alpha(255);
	tCache.m_bBox = pGroup->m_iESP & ESPEnum::Box;
	tCache.m_bBones = pGroup->m_iESP & ESPEnum::Bones;

	if (pGroup->m_iESP & ESPEnum::Distance && !bLocal)
	{
		Vec3 vDelta = pPlayer->m_vecOrigin() - pLocal->m_vecOrigin();
		tCache.m_vText.emplace_back(ALIGN_BOTTOM, std::format("[{:.0f}M]", vDelta.Length2D() / 41), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pResource)
	{
		if (pGroup->m_iESP & ESPEnum::Name)
			tCache.m_vText.emplace_back(ALIGN_TOP, F::PlayerUtils.GetPlayerName(iIndex, pResource->GetName(iIndex)), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (pGroup->m_iESP & (ESPEnum::Labels | ESPEnum::Priority) && !pResource->IsFakePlayer(iIndex))
		{
			uint32_t uAccountID = pResource->m_iAccountID(iIndex);

			if (pGroup->m_iESP & ESPEnum::Priority)
			{
				if (auto pTag = F::PlayerUtils.GetSignificantTag(uAccountID, 1))
					tCache.m_vText.emplace_back(ALIGN_TOP, pTag->m_sName, pTag->m_tColor, pTag->m_tColor.IsColorDark() ? Color_t(255, 255, 255) : Color_t(0, 0, 0));
			}

			if (pGroup->m_iESP & ESPEnum::Labels)
			{
				std::vector<std::tuple<std::string, Color_t, int>> vTags = {};
				vTags.reserve(F::PlayerUtils.GetPlayerTags(uAccountID).size() + 3); // +friend/party/f2p
				for (auto& iID : F::PlayerUtils.GetPlayerTags(uAccountID))
				{
					auto pTag = F::PlayerUtils.GetTag(iID);
					if (pTag && pTag->m_bLabel)
						vTags.emplace_back(pTag->m_sName, pTag->m_tColor, pTag->m_iPriority);
				}
				if (H::Entities.IsFriend(uAccountID))
				{
					auto pTag = &F::PlayerUtils.m_vTags[F::PlayerUtils.TagToIndex(FRIEND_TAG)];
					if (pTag->m_bLabel)
						vTags.emplace_back(pTag->m_sName, pTag->m_tColor, pTag->m_iPriority);
				}
				if (auto iParty = H::Entities.GetParty(uAccountID))
				{
					auto pTag = &F::PlayerUtils.m_vTags[F::PlayerUtils.TagToIndex(PARTY_TAG)];
					if (int iPartyCount = H::Entities.GetPartyCount() + 1; pTag->m_bLabel)
					{
						if (!--iParty)
							vTags.emplace_back(pTag->m_sName, pTag->m_tColor, pTag->m_iPriority);
						else
							vTags.emplace_back(std::format("{}: {}", pTag->m_sName, iParty), pTag->m_tColor.HueShift(iParty * 360.f / iPartyCount), pTag->m_iPriority);
					}
				}
				if (H::Entities.IsF2P(uAccountID))
				{
					auto pTag = &F::PlayerUtils.m_vTags[F::PlayerUtils.TagToIndex(F2P_TAG)];
					if (pTag->m_bLabel)
						vTags.emplace_back(pTag->m_sName, pTag->m_tColor, pTag->m_iPriority);
				}

				if (!vTags.empty())
				{
					std::sort(vTags.begin(), vTags.end(), [&](const auto a, const auto b) -> bool
					{
						// sort by priority if unequal
						if (std::get<2>(a) != std::get<2>(b))
							return std::get<2>(a) > std::get<2>(b);

						return std::get<0>(a) < std::get<0>(b);
					});

					for (auto& [sName, tColor, _] : vTags)
						tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, sName, tColor, tColor.IsColorDark() ? Color_t(255, 255, 255) : Color_t(0, 0, 0));
				}
			}
		}
	}

	float flHealth = pPlayer->m_iHealth(), flMaxHealth = pPlayer->GetMaxHealth();
	if (pGroup->m_iESP & ESPEnum::HealthBar)
	{
		tCache.m_flHealth = flHealth > flMaxHealth
			? 1.f + std::clamp((flHealth - flMaxHealth) / (floorf(flMaxHealth / 10.f) * 5), 0.f, 1.f)
			: std::clamp(flHealth / flMaxHealth, 0.f, 1.f);
		Color_t tColor = Vars::Colors::IndicatorBad.Value.Lerp(Vars::Colors::IndicatorGood.Value, std::clamp(tCache.m_flHealth, 0.f, 1.f), LerpEnum::HSV);
		tCache.m_vBars.emplace_back(ALIGN_LEFT, tCache.m_flHealth, tColor, Vars::Colors::IndicatorMisc.Value);
	}
	if (pGroup->m_iESP & ESPEnum::HealthText)
		tCache.m_vText.emplace_back(ALIGN_LEFT, std::format("{}", flHealth), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

	if (pGroup->m_iESP & (ESPEnum::UberBar | ESPEnum::UberText) && iClassNum == TF_CLASS_MEDIC)
	{
		auto pMediGun = pPlayer->GetWeaponFromSlot(SLOT_SECONDARY);
		if (pMediGun && pMediGun->GetClassID() == ETFClassID::CWeaponMedigun)
		{
			float flUber = std::clamp(pMediGun->As<CWeaponMedigun>()->m_flChargeLevel(), 0.f, 1.f);
			if (pGroup->m_iESP & ESPEnum::UberBar)
				tCache.m_vBars.emplace_back(ALIGN_BOTTOM, flUber, Vars::Colors::IndicatorMisc.Value, Color_t(), false);
			if (pGroup->m_iESP & ESPEnum::UberText)
				tCache.m_vText.emplace_back(ALIGN_BOTTOMRIGHT, std::format("{:.0f}%", flUber * 100), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		}
	}

	if (pGroup->m_iESP & ESPEnum::ClassIcon)
		tCache.m_iClassIcon = iClassNum;
	if (pGroup->m_iESP & ESPEnum::ClassText)
		tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, SDK::GetClassByIndex(iClassNum, false), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

	if (pGroup->m_iESP & ESPEnum::WeaponIcon && pWeapon)
		tCache.m_pWeaponIcon = pWeapon->GetWeaponIcon();
	if (pGroup->m_iESP & ESPEnum::WeaponText && pWeapon)
		tCache.m_vText.emplace_back(ALIGN_BOTTOM, SDK::ConvertWideToUTF8(pWeapon->GetWeaponName()), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

	if (pGroup->m_iESP & ESPEnum::LagCompensation && !pPlayer->IsDormant() && !bLocal)
	{
		if (H::Entities.GetLagCompensation(iIndex))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Lagcomp", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::Ping && pResource && !bLocal)
	{
		int iPing = pResource->m_iPing(iIndex);
		if (iPing && (iPing >= 200 || iPing <= 5))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("{}MS", iPing), Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::KDR && pResource && !bLocal)
	{
		int iKills = pResource->m_iScore(iIndex), iDeaths = pResource->m_iDeaths(iIndex);
		if (iKills >= 20)
		{
			int iKDR = iKills / std::max(iDeaths, 1);
			if (iKDR >= 10)
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("High KD [{} / {}]", iKills, iDeaths), Vars::Colors::IndicatorTextMid.Value, Vars::Menu::Theme::Background.Value);
		}
	}

	// The Buffs/Debuffs/state sections below issue dozens of InCond() queries per
	// player, each re-reading the same condition dwords through the netvar
	// accessors. Snapshot the five dwords once and test bits locally, mirroring
	// CTFPlayer::InCond exactly (word 0 folds in _condition_bits like InCond does).
	const int aCond[5] = {
		pPlayer->m_nPlayerCond() | pPlayer->_condition_bits(),
		pPlayer->m_nPlayerCondEx(),
		pPlayer->m_nPlayerCondEx2(),
		pPlayer->m_nPlayerCondEx3(),
		pPlayer->m_nPlayerCondEx4(),
	};
	const auto bInCond = [&](ETFCond eCond) -> bool
	{
		const int iWord = eCond / 32;
		return iWord >= 0 && iWord < 5 && (aCond[iWord] & (1 << (eCond - iWord * 32)));
	};

	// Buffs
	if (pGroup->m_iESP & ESPEnum::Buffs)
	{
		if (bInCond(TF_COND_INVULNERABLE) ||
			bInCond(TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED) ||
			bInCond(TF_COND_INVULNERABLE_USER_BUFF) ||
			bInCond(TF_COND_INVULNERABLE_CARD_EFFECT))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Uber", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_MEGAHEAL))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Megaheal", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_PHASE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Bonk", Vars::Colors::IndicatorTextMid.Value, Vars::Menu::Theme::Background.Value);

		bool bCrits = pPlayer->IsCritBoosted(), bMiniCrits = pPlayer->IsMiniCritBoosted();
		if (pWeapon)
		{
			if (bMiniCrits && SDK::AttribHookValue(0, "minicrits_become_crits", pWeapon)
				|| bInCond(TF_COND_BLASTJUMPING) && SDK::AttribHookValue(0, "crit_while_airborne", pWeapon))
				bCrits = true, bMiniCrits = false;
			if (bCrits && SDK::AttribHookValue(0, "crits_become_minicrits", pWeapon))
				bCrits = false, bMiniCrits = true;
		}
		if (bCrits)
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Crits", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		else if (bMiniCrits)
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Mini-crits", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);

		/* vaccinator effects */
		if (bInCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) || bInCond(TF_COND_BULLET_IMMUNE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Bullet+", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_MEDIGUN_SMALL_BULLET_RESIST))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Bullet", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST) || bInCond(TF_COND_BLAST_IMMUNE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Blast+", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_MEDIGUN_SMALL_BLAST_RESIST))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Blast", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST) || bInCond(TF_COND_FIRE_IMMUNE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Fire+", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_MEDIGUN_SMALL_FIRE_RESIST))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Fire", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_OFFENSEBUFF))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Banner", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_DEFENSEBUFF))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Battalions", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_REGENONDAMAGEBUFF))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Conch", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_RUNE_STRENGTH))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Strength", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_HASTE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Haste", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_REGEN))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Regen", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_RESIST))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Resistance", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_VAMPIRE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Vampire", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_REFLECT))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Reflect", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_PRECISION))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Precision", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_AGILITY))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Agility", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_KNOCKOUT))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Knockout", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_KING))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "King", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_PLAGUE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Plague", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_RUNE_SUPERNOVA))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Supernova", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		if (bInCond(TF_COND_POWERUPMODE_DOMINANT))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Dominant", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		for (int i = 0; i < MAX_WEAPONS; i++)
		{
			auto pWeapon = pPlayer->GetWeaponFromSlot(i)->As<CTFSpellBook>();
			if (!pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_SPELLBOOK || !pWeapon->m_iSpellCharges())
				continue;

			switch (pWeapon->m_iSelectedSpellIndex())
			{
			case 0: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Fireball", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 1: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Bats", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 2: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Heal", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 3: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Pumpkins", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 4: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Jump", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 5: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Stealth", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 6: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Teleport", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 7: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Lightning", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 8: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Minify", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 9: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Meteors", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 10: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Monoculus", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 11: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Skeletons", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 12: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Glove", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 13: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Parachute", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 14: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Heal", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			case 15: tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Bomb", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value); break;
			}
		}

		if (bInCond(TF_COND_RADIUSHEAL) ||
			bInCond(TF_COND_HEALTH_BUFF) ||
			bInCond(TF_COND_RADIUSHEAL_ON_DAMAGE) ||
			bInCond(TF_COND_HALLOWEEN_QUICK_HEAL) ||
			bInCond(TF_COND_HALLOWEEN_HELL_HEAL) ||
			bInCond(TF_COND_KING_BUFFED))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Heal", Vars::Colors::IndicatorTextGood.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_HEALTH_OVERHEALED))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "HP+", Vars::Colors::IndicatorTextGood.Value, Vars::Menu::Theme::Background.Value);

		//if (bInCond(TF_COND_BLASTJUMPING))
		//	tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Blastjump", Vars::Colors::IndicatorTextMid.Value, Vars::Menu::Theme::Background.Value);
	}

	// Debuffs
	if (pGroup->m_iESP & ESPEnum::Debuffs)
	{
		if (bInCond(TF_COND_MARKEDFORDEATH)
			|| bInCond(TF_COND_MARKEDFORDEATH_SILENT)
			|| bInCond(TF_COND_PASSTIME_PENALTY_DEBUFF))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Marked", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_URINE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Jarate", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_MAD_MILK))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Milk", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_STUNNED))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Stun", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_BURNING))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Burn", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_BLEEDING))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Bleed", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	// Misc
	if (pGroup->m_iESP & ESPEnum::Flags)
	{
		if (pPlayer->m_bFeignDeathReady())
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "DR", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		else if (bInCond(TF_COND_FEIGN_DEATH))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Feign", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (float flInvis = pPlayer->GetEffectiveInvisibilityLevel())
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Invis {:.0f}%", flInvis * 100), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_DISGUISED))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Disguise", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (bInCond(TF_COND_AIMING) || bInCond(TF_COND_ZOOMED))
		{
			switch (pWeapon ? pWeapon->GetWeaponID() : -1)
			{
			case TF_WEAPON_MINIGUN:
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Rev", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
				break;
			case TF_WEAPON_SNIPERRIFLE:
			case TF_WEAPON_SNIPERRIFLE_CLASSIC:
			case TF_WEAPON_SNIPERRIFLE_DECAP:
			{
				if (bLocal)
				{
					tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Charging {:.0f}%", Math::RemapVal(pWeapon->As<CTFSniperRifle>()->m_flChargedDamage(), 0.f, 150.f, 0.f, 100.f)), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
					break;
				}
				else
				{
					auto fGetSniperDot = [](CBaseEntity* pEntity) -> CSniperDot*
					{
						for (auto pDot : H::Entities.GetGroup(EntityEnum::SniperDots))
						{
							if (pDot->m_hOwnerEntity().Get() == pEntity)
								return pDot->As<CSniperDot>();
						}
						return nullptr;
					};
					if (CSniperDot* pPlayerDot = fGetSniperDot(pPlayer))
					{
						float flChargeTime = std::max(SDK::AttribHookValue(3.f, "mult_sniper_charge_per_sec", pWeapon), 1.5f);
						tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Charging {:.0f}%", Math::RemapVal(TICKS_TO_TIME(I::ClientState->m_ClockDriftMgr.m_nServerTick) - pPlayerDot->m_flChargeStartTime() - 0.3f, 0.f, flChargeTime, 0.f, 100.f)), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
						break;
					}
				}
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Charging", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
				break;
			}
			case TF_WEAPON_COMPOUND_BOW:
				if (bLocal)
				{
					tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Charging {:.0f}%", Math::RemapVal(TICKS_TO_TIME(I::ClientState->m_ClockDriftMgr.m_nServerTick) - pWeapon->As<CTFPipebombLauncher>()->m_flChargeBeginTime(), 0.f, 1.f, 0.f, 100.f)), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
					break;
				}
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Charging", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
				break;
			default:
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Charging", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
			}
		}

		if (bInCond(TF_COND_SHIELD_CHARGE))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Charging", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (Vars::Visuals::Removals::Taunts.Value && bInCond(TF_COND_TAUNTING))
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Taunt", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (Vars::Debug::Info.Value && !bLocal /*&& !pPlayer->IsDormant()*/)
		{
			int iAverage = TIME_TO_TICKS(F::MoveSim.GetPredictedDelta(pPlayer));
			int iCurrent = H::Entities.GetChoke(iIndex);
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Lag {}, {}", iAverage, iCurrent), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		}
	}
}

static inline void StoreBuilding(CBaseObject* pBuilding, CTFPlayer* pLocal, Group_t* pGroup, std::unordered_map<CBaseEntity*, BuildingCache_t>& mCache)
{
	auto pOwner = pBuilding->m_hBuilder().Get();
	int iIndex = pOwner ? pOwner->entindex() : -1;

	bool bIsMini = pBuilding->m_bMiniBuilding();

	BuildingCache_t& tCache = mCache[pBuilding];
	tCache.m_flAlpha = pGroup->m_tColor.a / 255.f;
	tCache.m_tColor = F::Groups.GetColor(pOwner ? pOwner : pBuilding, pGroup).Alpha(255);
	tCache.m_bBox = pGroup->m_iESP & ESPEnum::Box;

	if (pGroup->m_iESP & ESPEnum::Distance)
	{
		Vec3 vDelta = pBuilding->m_vecOrigin() - pLocal->m_vecOrigin();
		tCache.m_vText.emplace_back(ALIGN_BOTTOM, std::format("[{:.0f}M]", vDelta.Length2D() / 41), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::Name)
	{
		const char* sName = "Building";
		switch (pBuilding->GetClassID())
		{
		case ETFClassID::CObjectSentrygun: sName = bIsMini ? "Mini-Sentry" : "Sentry"; break;
		case ETFClassID::CObjectDispenser: sName = "Dispenser"; break;
		case ETFClassID::CObjectTeleporter: sName = pBuilding->m_iObjectMode() ? "Teleporter Exit" : "Teleporter Entrance";
		}
		tCache.m_vText.emplace_back(ALIGN_TOP, sName, Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	float flHealth = pBuilding->m_iHealth(), flMaxHealth = pBuilding->m_iMaxHealth();
	if (pGroup->m_iESP & ESPEnum::HealthBar)
	{
		tCache.m_flHealth = std::clamp(flHealth / flMaxHealth, 0.f, 1.f);
		Color_t tColor = Vars::Colors::IndicatorBad.Value.Lerp(Vars::Colors::IndicatorGood.Value, std::clamp(tCache.m_flHealth, 0.f, 1.f), LerpEnum::HSV);
		tCache.m_vBars.emplace_back(ALIGN_LEFT, tCache.m_flHealth, tColor, Vars::Colors::IndicatorMisc.Value);
	}
	if (pGroup->m_iESP & ESPEnum::HealthText)
		tCache.m_vText.emplace_back(ALIGN_LEFT, std::format("{}", flHealth), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

	if (pGroup->m_iESP & (ESPEnum::AmmoBars | ESPEnum::AmmoText) && pBuilding->IsSentrygun() && !pBuilding->m_bBuilding())
	{
		int iShells, iMaxShells, iRockets, iMaxRockets; pBuilding->As<CObjectSentrygun>()->GetAmmoCount(iShells, iMaxShells, iRockets, iMaxRockets);

		if (pGroup->m_iESP & ESPEnum::AmmoBars)
		{
			tCache.m_vBars.emplace_back(ALIGN_BOTTOM, float(iShells) / iMaxShells, Vars::Menu::Theme::Inactive.Value, Color_t(), false);
			if (iMaxRockets)
				tCache.m_vBars.emplace_back(ALIGN_BOTTOM, float(iRockets) / iMaxRockets, Vars::Menu::Theme::Inactive.Value, Color_t(), false);
		}
		if (pGroup->m_iESP & ESPEnum::AmmoText)
		{
			tCache.m_vText.emplace_back(ALIGN_BOTTOMRIGHT, std::format("{}", iShells), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
			if (iMaxRockets)
				tCache.m_vText.back().m_sText += std::format(", {}", iRockets);
		}
	}

	if (pGroup->m_iESP & ESPEnum::Owner && !pBuilding->m_bWasMapPlaced() && pOwner)
	{
		if (auto pResource = H::Entities.GetResource(); pResource)
			tCache.m_vText.emplace_back(ALIGN_TOP, F::PlayerUtils.GetPlayerName(iIndex, pResource->GetName(iIndex)), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::Level && !bIsMini)
		tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Level {}", pBuilding->m_iUpgradeLevel()), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

	if (pGroup->m_iESP & ESPEnum::Flags)
	{
		if (!pBuilding->IsDormant() && pBuilding->m_bBuilding())
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("{:.0f}%", pBuilding->m_flPercentageConstructed() * 100), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (pBuilding->IsSentrygun() && pBuilding->As<CObjectSentrygun>()->m_bPlayerControlled())
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Wrangled", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);

		if (pBuilding->m_bHasSapper())
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Sapped", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		else if (pBuilding->m_bDisabled())
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Disabled", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}
}

static inline const char* GetProjectileName(CBaseEntity* pProjectile)
{
	const char* sReturn = "Projectile";
	switch (pProjectile->GetClassID())
	{
	case ETFClassID::CTFWeaponBaseMerasmusGrenade: sReturn = "Bomb"; break;
	case ETFClassID::CTFGrenadePipebombProjectile: sReturn = pProjectile->As<CTFGrenadePipebombProjectile>()->HasStickyEffects() ? "Sticky" : "Pipe"; break;
	case ETFClassID::CTFStunBall: sReturn = "Baseball"; break;
	case ETFClassID::CTFBall_Ornament: sReturn = "Bauble"; break;
	case ETFClassID::CTFProjectile_Jar: sReturn = "Jarate"; break;
	case ETFClassID::CTFProjectile_Cleaver: sReturn = "Cleaver"; break;
	case ETFClassID::CTFProjectile_JarGas: sReturn = "Gas"; break;
	case ETFClassID::CTFProjectile_JarMilk:
	case ETFClassID::CTFProjectile_ThrowableBreadMonster: sReturn = "Milk"; break;
	case ETFClassID::CTFProjectile_SpellBats:
	case ETFClassID::CTFProjectile_SpellKartBats: sReturn = "Bats"; break;
	case ETFClassID::CTFProjectile_SpellMeteorShower: sReturn = "Meteors"; break;
	case ETFClassID::CTFProjectile_SpellMirv:
	case ETFClassID::CTFProjectile_SpellPumpkin: sReturn = "Pumpkin"; break;
	case ETFClassID::CTFProjectile_SpellSpawnBoss: sReturn = "Monoculus"; break;
	case ETFClassID::CTFProjectile_SpellSpawnHorde:
	case ETFClassID::CTFProjectile_SpellSpawnZombie: sReturn = "Skeleton"; break;
	case ETFClassID::CTFProjectile_SpellTransposeTeleport: sReturn = "Teleport"; break;
	case ETFClassID::CTFProjectile_Arrow: sReturn = pProjectile->As<CTFProjectile_Arrow>()->m_iProjectileType() == TF_PROJECTILE_BUILDING_REPAIR_BOLT ? "Repair" : "Arrow"; break;
	case ETFClassID::CTFProjectile_GrapplingHook: sReturn = "Grapple"; break;
	case ETFClassID::CTFProjectile_HealingBolt: sReturn = "Heal"; break;
	case ETFClassID::CTFProjectile_Rocket:
	case ETFClassID::CTFProjectile_EnergyBall:
	case ETFClassID::CTFProjectile_SentryRocket: sReturn = "Rocket"; break;
	case ETFClassID::CTFProjectile_BallOfFire: sReturn = "Fire"; break;
	case ETFClassID::CTFProjectile_MechanicalArmOrb: sReturn = "Short circuit"; break;
	case ETFClassID::CTFProjectile_SpellFireball: sReturn = "Fireball"; break;
	case ETFClassID::CTFProjectile_SpellLightningOrb: sReturn = "Lightning"; break;
	case ETFClassID::CTFProjectile_SpellKartOrb: sReturn = "Fist"; break;
	case ETFClassID::CTFProjectile_Flare: sReturn = "Flare"; break;
	case ETFClassID::CTFProjectile_EnergyRing: sReturn = "Energy"; break;
	}
	return sReturn;
}
static inline void StoreProjectile(CBaseEntity* pProjectile, CTFPlayer* pLocal, Group_t* pGroup, std::unordered_map<CBaseEntity*, EntityCache_t>& mCache)
{
	auto pOwner = F::ProjSim.GetEntities(pProjectile).second;
	int iIndex = pOwner ? pOwner->entindex() : -1;

	EntityCache_t& tCache = mCache[pProjectile];
	tCache.m_flAlpha = pGroup->m_tColor.a / 255.f;
	tCache.m_tColor = F::Groups.GetColor(pOwner ? pOwner : pProjectile, pGroup);
	tCache.m_bBox = pGroup->m_iESP & ESPEnum::Box;

	if (pGroup->m_iESP & ESPEnum::Distance)
	{
		Vec3 vDelta = pProjectile->m_vecOrigin() - pLocal->m_vecOrigin();
		tCache.m_vText.emplace_back(ALIGN_BOTTOM, std::format("[{:.0f}M]", vDelta.Length2D() / 41), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::Name)
		tCache.m_vText.emplace_back(ALIGN_TOP, GetProjectileName(pProjectile), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

	if (pGroup->m_iESP & ESPEnum::Owner && pOwner)
	{
		if (auto pResource = H::Entities.GetResource(); pResource)
			tCache.m_vText.emplace_back(ALIGN_TOP, F::PlayerUtils.GetPlayerName(iIndex, pResource->GetName(iIndex)), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::Flags)
	{
		switch (pProjectile->GetClassID())
		{
		case ETFClassID::CTFWeaponBaseGrenadeProj:
		case ETFClassID::CTFWeaponBaseMerasmusGrenade:
		case ETFClassID::CTFGrenadePipebombProjectile:
		case ETFClassID::CTFStunBall:
		case ETFClassID::CTFBall_Ornament:
		case ETFClassID::CTFProjectile_Jar:
		case ETFClassID::CTFProjectile_Cleaver:
		case ETFClassID::CTFProjectile_JarGas:
		case ETFClassID::CTFProjectile_JarMilk:
		case ETFClassID::CTFProjectile_SpellBats:
		case ETFClassID::CTFProjectile_SpellKartBats:
		case ETFClassID::CTFProjectile_SpellMeteorShower:
		case ETFClassID::CTFProjectile_SpellMirv:
		case ETFClassID::CTFProjectile_SpellPumpkin:
		case ETFClassID::CTFProjectile_SpellSpawnBoss:
		case ETFClassID::CTFProjectile_SpellSpawnHorde:
		case ETFClassID::CTFProjectile_SpellSpawnZombie:
		case ETFClassID::CTFProjectile_SpellTransposeTeleport:
		case ETFClassID::CTFProjectile_Throwable:
		case ETFClassID::CTFProjectile_ThrowableBreadMonster:
		case ETFClassID::CTFProjectile_ThrowableBrick:
		case ETFClassID::CTFProjectile_ThrowableRepel:
			if (pProjectile->As<CTFWeaponBaseGrenadeProj>()->m_bCritical())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Crit", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			if (pProjectile->As<CTFWeaponBaseGrenadeProj>()->m_iDeflected() && (pProjectile->GetClassID() != ETFClassID::CTFGrenadePipebombProjectile || !pProjectile->GetAbsVelocity().IsZero()))
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Reflected", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			break;
		case ETFClassID::CTFProjectile_Arrow:
		case ETFClassID::CTFProjectile_GrapplingHook:
		case ETFClassID::CTFProjectile_HealingBolt:
			if (pProjectile->As<CTFProjectile_Arrow>()->m_bCritical())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Crit", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Reflected", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			if (pProjectile->As<CTFProjectile_Arrow>()->m_bArrowAlight())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Alight", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			break;
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_BallOfFire:
		case ETFClassID::CTFProjectile_MechanicalArmOrb:
		case ETFClassID::CTFProjectile_SentryRocket:
		case ETFClassID::CTFProjectile_SpellFireball:
		case ETFClassID::CTFProjectile_SpellLightningOrb:
		case ETFClassID::CTFProjectile_SpellKartOrb:
			if (pProjectile->As<CTFProjectile_Rocket>()->m_bCritical())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Crit", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Reflected", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			break;
		case ETFClassID::CTFProjectile_EnergyBall:
			if (pProjectile->As<CTFProjectile_EnergyBall>()->m_bChargedShot())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Charge", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Reflected", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			break;
		case ETFClassID::CTFProjectile_Flare:
			if (pProjectile->As<CTFProjectile_Flare>()->m_bCritical())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Crit", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Reflected", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			break;
		}
	}
}

static inline void StoreObjective(CBaseEntity* pObjective, CTFPlayer* pLocal, Group_t* pGroup, std::unordered_map<CBaseEntity*, EntityCache_t>& mCache)
{
	auto pOwner = pObjective->m_hOwnerEntity()->As<CTFPlayer>();
	if (pOwner == pLocal)
		return;

	EntityCache_t& tCache = mCache[pObjective];
	tCache.m_flAlpha = pGroup->m_tColor.a / 255.f;
	tCache.m_tColor = F::Groups.GetColor(pObjective, pGroup);
	tCache.m_bBox = pGroup->m_iESP & ESPEnum::Box;

	if (pGroup->m_iESP & ESPEnum::Distance)
	{
		Vec3 vDelta = pObjective->m_vecOrigin() - pLocal->m_vecOrigin();
		tCache.m_vText.emplace_back(ALIGN_BOTTOM, std::format("[{:.0f}M]", vDelta.Length2D() / 41), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	switch (pObjective->GetClassID())
	{
	case ETFClassID::CCaptureFlag:
	{
		auto pIntel = pObjective->As<CCaptureFlag>();

		if (pGroup->m_iESP & ESPEnum::Name)
			tCache.m_vText.emplace_back(ALIGN_TOP, "Intel", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);

		if (pGroup->m_iESP & ESPEnum::Flags)
		{
			switch (pIntel->m_nFlagStatus())
			{
			case TF_FLAGINFO_HOME:
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Home", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
				break;
			case TF_FLAGINFO_DROPPED:
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Dropped", Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
				break;
			default:
				tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, "Stolen", Vars::Colors::IndicatorTextBad.Value, Vars::Menu::Theme::Background.Value);
			}
		}

		if (pGroup->m_iESP & ESPEnum::IntelReturnTime && pIntel->m_nFlagStatus() == TF_FLAGINFO_DROPPED)
		{
			float flReturnTime = std::max(pIntel->m_flResetTime() - TICKS_TO_TIME(I::ClientState->m_ClockDriftMgr.m_nServerTick), 0.f);
			tCache.m_vText.emplace_back(ALIGN_TOPRIGHT, std::format("Return {:.1f}s", pIntel->m_flResetTime() - TICKS_TO_TIME(I::ClientState->m_ClockDriftMgr.m_nServerTick)).c_str(), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
		}

		break;
	}
	}
}

static inline void StoreMisc(CBaseEntity* pEntity, CTFPlayer* pLocal, Group_t* pGroup, std::unordered_map<CBaseEntity*, EntityCache_t>& mCache)
{
	EntityCache_t& tCache = mCache[pEntity];
	tCache.m_flAlpha = pGroup->m_tColor.a / 255.f;
	tCache.m_tColor = F::Groups.GetColor(pEntity, pGroup);
	tCache.m_bBox = pGroup->m_iESP & ESPEnum::Box;

	if (pGroup->m_iESP & ESPEnum::Distance)
	{
		Vec3 vDelta = pEntity->m_vecOrigin() - pLocal->m_vecOrigin();
		tCache.m_vText.emplace_back(ALIGN_BOTTOM, std::format("[{:.0f}M]", vDelta.Length2D() / 41), Vars::Menu::Theme::Active.Value, Vars::Menu::Theme::Background.Value);
	}

	if (pGroup->m_iESP & ESPEnum::Name)
	{
		const char* sName = "Unknown";
		switch (pEntity->GetClassID())
		{
		case ETFClassID::CTFBaseBoss: sName = "NPC"; break;
		case ETFClassID::CTFTankBoss: sName = "Tank"; break;
		case ETFClassID::CMerasmus: sName = "Merasmus"; break;
		case ETFClassID::CEyeballBoss: sName = "Monoculus"; break;
		case ETFClassID::CHeadlessHatman: sName = "Horseless Headless Horsemann"; break;
		case ETFClassID::CZombie: sName = "Skeleton"; break;
		case ETFClassID::CBaseAnimating:
		{
			auto uHash = H::Entities.GetModel(pEntity->entindex());
			if (H::Entities.IsHealth(uHash))
				sName = "Health";
			else if (H::Entities.IsAmmo(uHash))
				sName = "Ammo";
			else if (H::Entities.IsSpellbook(uHash))
				sName = "Spellbook";
			else if (H::Entities.IsPowerup(uHash))
			{
				sName = "Powerup";
				switch (uHash)
				{
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_agility.mdl"): sName = "Agility"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_crit.mdl"): sName = "Revenge"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_defense.mdl"): sName = "Resistance"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_haste.mdl"): sName = "Haste"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_king.mdl"): sName = "King"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_knockout.mdl"): sName = "Knockout"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_plague.mdl"): sName = "Plague"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_precision.mdl"): sName = "Precision"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_reflect.mdl"): sName = "Reflect"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_regen.mdl"): sName = "Regeneration"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_strength.mdl"): sName = "Strength"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_supernova.mdl"): sName = "Supernova"; break;
				case FNV1A::Hash32Const("models/pickups/pickup_powerup_vampire.mdl"): sName = "Vampire";
				}
			}
			break;
		}
		case ETFClassID::CTFAmmoPack: sName = "Ammo"; break;
		case ETFClassID::CCurrencyPack: sName = "Money"; break;
		case ETFClassID::CTFGenericBomb:
		case ETFClassID::CTFPumpkinBomb: sName = "Bomb"; break;
		case ETFClassID::CHalloweenGiftPickup: sName = "Gargoyle"; break;
		}

		tCache.m_vText.emplace_back(ALIGN_TOP, sName, pGroup->m_tColor, Vars::Menu::Theme::Background.Value);
	}
}

// Reset a cache entry to a default-constructed state (so every scalar field is
// back to its default exactly as a fresh insert would be) while REUSING the
// existing vectors' heap buffers instead of freeing and reallocating them. This
// keeps behavior identical to the old clear()+fresh-insert while eliminating the
// per-tick allocation churn. Stamped with the current store generation so stale
// entries (entities no longer tracked) can be pruned afterward.
template <typename T>
static inline T& ResetCacheEntry(std::unordered_map<CBaseEntity*, T>& mCache, CBaseEntity* pEntity, uint32_t uStamp)
{
	T& tCache = mCache[pEntity];

	// Salvage the vector buffers, reset the struct to defaults, move buffers back.
	std::vector<Text_t> vText = std::move(tCache.m_vText);
	vText.clear();
	if constexpr (requires { tCache.m_vBars; })
	{
		std::vector<Bar_t> vBars = std::move(tCache.m_vBars);
		vBars.clear();
		tCache = T{};
		tCache.m_vBars = std::move(vBars);
	}
	else
		tCache = T{};
	tCache.m_vText = std::move(vText);

	tCache.m_uStamp = uStamp;
	return tCache;
}

template <typename T>
static inline void PruneStaleEntries(std::unordered_map<CBaseEntity*, T>& mCache, uint32_t uStamp)
{
	for (auto it = mCache.begin(); it != mCache.end();)
		it = (it->second.m_uStamp != uStamp) ? mCache.erase(it) : std::next(it);
}

void CESP::Store(CTFPlayer* pLocal)
{
	if (!pLocal || !F::Groups.GroupsActive())
	{
		m_mPlayerCache.clear();
		m_mBuildingCache.clear();
		m_mEntityCache.clear();
		return;
	}

	const uint32_t uStamp = ++m_uStoreStamp;

	for (auto& [pEntity, pGroup] : F::Groups.GetGroup(false))
	{
		if (!pGroup->m_iESP)
			continue;

		// Reset+stamp the entry up front so the Store* helpers append into a
		// cleared-but-capacity-retaining vector instead of a freshly allocated one.
		if (pEntity->IsPlayer())
		{
			ResetCacheEntry(m_mPlayerCache, pEntity, uStamp);
			StorePlayer(pEntity->As<CTFPlayer>(), pLocal, pGroup, m_mPlayerCache);
		}
		else if (pEntity->IsBuilding())
		{
			ResetCacheEntry(m_mBuildingCache, pEntity, uStamp);
			StoreBuilding(pEntity->As<CBaseObject>(), pLocal, pGroup, m_mBuildingCache);
		}
		else if (pEntity->IsProjectile())
		{
			ResetCacheEntry(m_mEntityCache, pEntity, uStamp);
			StoreProjectile(pEntity, pLocal, pGroup, m_mEntityCache);
		}
		else if (pEntity->GetClassID() == ETFClassID::CCaptureFlag)
		{
			ResetCacheEntry(m_mEntityCache, pEntity, uStamp);
			StoreObjective(pEntity, pLocal, pGroup, m_mEntityCache);
		}
		else
		{
			ResetCacheEntry(m_mEntityCache, pEntity, uStamp);
			StoreMisc(pEntity, pLocal, pGroup, m_mEntityCache);
		}
	}

	// Drop entries for entities that were not stored this tick (left the group /
	// went dormant / despawned), keeping only live slots and their capacity.
	PruneStaleEntries(m_mPlayerCache, uStamp);
	PruneStaleEntries(m_mBuildingCache, uStamp);
	PruneStaleEntries(m_mEntityCache, uStamp);
}

static matrix3x4 s_aBones[MAXSTUDIOBONES];
static matrix3x4 s_mTransform = {};

void CESP::Draw()
{
	Math::AngleMatrix({ 0.f, I::EngineClient->GetViewAngles().y, 0.f }, s_mTransform, false);

	DrawWorld();
	DrawBuildings();
	DrawPlayers();
}

void CESP::DrawPlayers()
{
	if (m_mPlayerCache.empty())
		return;

	const auto& fFont = H::Fonts.GetFont(FONT_ESP);
	const int nTall = fFont.m_nTall + H::Draw.Scale(2);
	// Loop-invariant scaled paddings: Scale() reads the menu-scale ConfigVar and
	// rounds; the args are constant, so resolve them once instead of per player.
	const int iPad6 = H::Draw.Scale(6), iPad5 = H::Draw.Scale(5), iPad2 = H::Draw.Scale(2);
	const int iBarSpace = H::Draw.Scale(4), iBarThickness = H::Draw.Scale(2, Scale_Round);
	const int iIconSize = H::Draw.Scale(18, Scale_Round);
	for (auto& [pEntity, tCache] : m_mPlayerCache)
	{
		float x, y, w, h;
		if (!GetDrawBounds(pEntity, x, y, w, h))
			continue;

		int l = x - iPad6, r = x + w + iPad6, m = x + w / 2;
		int t = y - iPad5, b = y + h + iPad5;
		int lOffset = 0, rOffset = 0, bOffset = 0, tOffset = 0;
		I::MatSystemSurface->DrawSetAlphaMultiplier(tCache.m_flAlpha);

		if (tCache.m_bBox)
			H::Draw.LineRectOutline(x, y, w, h, tCache.m_tColor, { 0, 0, 0, 255 });

		if (tCache.m_bBones)
		{
			auto pPlayer = pEntity->As<CTFPlayer>();
			if (pPlayer->SetupBones(s_aBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, I::GlobalVars->curtime))
			{
				// The model hash is constant across all 15 remaps; resolve it once
				// instead of letting each GetBaseToHitbox re-run GetModel(entindex()).
				const uint32_t uModel = H::Entities.GetModel(pPlayer->entindex());
				int iHead = pPlayer->GetBaseToHitbox(HITBOX_HEAD, uModel);
				int iSpine2 = pPlayer->GetBaseToHitbox(HITBOX_SPINE2, uModel);
				int iPelvis = pPlayer->GetBaseToHitbox(HITBOX_PELVIS, uModel);
				int iLeftUpperarm = pPlayer->GetBaseToHitbox(HITBOX_LEFT_UPPERARM, uModel);
				int iLeftForearm = pPlayer->GetBaseToHitbox(HITBOX_LEFT_FOREARM, uModel);
				int iLeftHand = pPlayer->GetBaseToHitbox(HITBOX_LEFT_HAND, uModel);
				int iRightUpperarm = pPlayer->GetBaseToHitbox(HITBOX_RIGHT_UPPERARM, uModel);
				int iRightForearm = pPlayer->GetBaseToHitbox(HITBOX_RIGHT_FOREARM, uModel);
				int iRightHand = pPlayer->GetBaseToHitbox(HITBOX_RIGHT_HAND, uModel);
				int iLeftThigh = pPlayer->GetBaseToHitbox(HITBOX_LEFT_THIGH, uModel);
				int iLeftCalf = pPlayer->GetBaseToHitbox(HITBOX_LEFT_CALF, uModel);
				int iLeftFoot = pPlayer->GetBaseToHitbox(HITBOX_LEFT_FOOT, uModel);
				int iRightThigh = pPlayer->GetBaseToHitbox(HITBOX_RIGHT_THIGH, uModel);
				int iRightCalf = pPlayer->GetBaseToHitbox(HITBOX_RIGHT_CALF, uModel);
				int iRightFoot = pPlayer->GetBaseToHitbox(HITBOX_RIGHT_FOOT, uModel);

				const int aSpine[] = { iHead, iSpine2, iPelvis };
				const int aLeftArm[] = { iSpine2, iLeftUpperarm, iLeftForearm, iLeftHand };
				const int aRightArm[] = { iSpine2, iRightUpperarm, iRightForearm, iRightHand };
				const int aLeftLeg[] = { iPelvis, iLeftThigh, iLeftCalf, iLeftFoot };
				const int aRightLeg[] = { iPelvis, iRightThigh, iRightCalf, iRightFoot };

				DrawBones(pPlayer, s_aBones, aSpine, tCache.m_tColor);
				DrawBones(pPlayer, s_aBones, aLeftArm, tCache.m_tColor);
				DrawBones(pPlayer, s_aBones, aRightArm, tCache.m_tColor);
				DrawBones(pPlayer, s_aBones, aLeftLeg, tCache.m_tColor);
				DrawBones(pPlayer, s_aBones, aRightLeg, tCache.m_tColor);
			}
		}

		for (auto& [iMode, flPercent, tColor, tOverfill, bAdjust] : tCache.m_vBars)
		{
			auto fDrawBar = [&](int x, int y, int w, int h, EAlign eAlign = ALIGN_LEFT)
			{
				if (flPercent > 1.f)
				{
					H::Draw.FillRectPercent(x, y, w, h, 1.f, tColor, { 0, 0, 0, 255 }, eAlign, bAdjust);
					H::Draw.FillRectPercent(x, y, w, h, flPercent - 1.f, tOverfill, { 0, 0, 0, 0 }, eAlign, bAdjust);
				}
				else
					H::Draw.FillRectPercent(x, y, w, h, flPercent, tColor, { 0, 0, 0, 255 }, eAlign, bAdjust);
			};

			const int iSpace = iBarSpace;
			const int iThickness = iBarThickness;
			switch (iMode)
			{
			case ALIGN_LEFT:
				fDrawBar(x - iSpace - iThickness - lOffset, y, iThickness, h, ALIGN_BOTTOM);
				lOffset += iSpace + iThickness;
				break;
			case ALIGN_BOTTOM:
				fDrawBar(x, y + h + iSpace + bOffset, w, iThickness);
				bOffset += iSpace + iThickness;
				break;
			}
		}

		for (auto& [iMode, sText, tColor, tOutline] : tCache.m_vText)
		{
			switch (iMode)
			{
			case ALIGN_TOP:
				H::Draw.StringOutlined(fFont, m, t - tOffset, tColor, tOutline, ALIGN_BOTTOM, sText.c_str());
				tOffset += nTall;
				break;
			case ALIGN_BOTTOM:
				H::Draw.StringOutlined(fFont, m, b + bOffset, tColor, tOutline, ALIGN_TOP, sText.c_str());
				bOffset += nTall;
				break;
			case ALIGN_LEFT:
				H::Draw.StringOutlined(fFont, l - lOffset, y - iPad2 + h - h * std::min(tCache.m_flHealth, 1.f), tColor, tOutline, ALIGN_TOPRIGHT, sText.c_str());
				break;
			case ALIGN_TOPRIGHT:
				H::Draw.StringOutlined(fFont, r, y - iPad2 + rOffset, tColor, tOutline, ALIGN_TOPLEFT, sText.c_str());
				rOffset += nTall;
				break;
			case ALIGN_BOTTOMRIGHT:
				H::Draw.StringOutlined(fFont, r, y + h, tColor, tOutline, ALIGN_TOPLEFT, sText.c_str());
				break;
			}
		}

		if (tCache.m_iClassIcon)
		{
			// Class -> leaderboard icon, table lookup instead of a per-player switch.
			static const char* const s_aClassIcons[] = {
				"vgui/glyph_multiplayer.vtf",              // 0 / TF_CLASS_UNDEFINED
				"hud/leaderboard_class_scout.vtf",         // TF_CLASS_SCOUT
				"hud/leaderboard_class_sniper.vtf",        // TF_CLASS_SNIPER
				"hud/leaderboard_class_soldier.vtf",       // TF_CLASS_SOLDIER
				"hud/leaderboard_class_demo.vtf",          // TF_CLASS_DEMOMAN
				"hud/leaderboard_class_medic.vtf",         // TF_CLASS_MEDIC
				"hud/leaderboard_class_heavy.vtf",         // TF_CLASS_HEAVY
				"hud/leaderboard_class_pyro.vtf",          // TF_CLASS_PYRO
				"hud/leaderboard_class_spy.vtf",           // TF_CLASS_SPY
				"hud/leaderboard_class_engineer.vtf",      // TF_CLASS_ENGINEER
			};
			const char* sTexture = (tCache.m_iClassIcon >= 0 && tCache.m_iClassIcon < int(std::size(s_aClassIcons)))
				? s_aClassIcons[tCache.m_iClassIcon] : "vgui/glyph_multiplayer.vtf";
			H::Draw.Texture(sTexture, m, t - tOffset, iIconSize, iIconSize, ALIGN_BOTTOM);
		}

		if (tCache.m_pWeaponIcon)
		{
			float flW = tCache.m_pWeaponIcon->Width(), flH = tCache.m_pWeaponIcon->Height();
			float flScale = H::Draw.Scale(std::min((w + 40) / 2.f, 80.f) / std::max(flW, flH * 2));
			H::Draw.DrawHudTexture(m - flW / 2.f * flScale, b + bOffset, flScale, tCache.m_pWeaponIcon, Vars::Menu::Theme::Active.Value);
		}
	}

	I::MatSystemSurface->DrawSetAlphaMultiplier(1.f);
}

void CESP::DrawBuildings()
{
	if (m_mBuildingCache.empty())
		return;

	const auto& fFont = H::Fonts.GetFont(FONT_ESP);
	const int nTall = fFont.m_nTall + H::Draw.Scale(2);
	for (auto& [pEntity, tCache] : m_mBuildingCache)
	{
		float x, y, w, h;
		if (!GetDrawBounds(pEntity, x, y, w, h))
			continue;

		int l = x - H::Draw.Scale(6), r = x + w + H::Draw.Scale(6), m = x + w / 2;
		int t = y - H::Draw.Scale(5), b = y + h + H::Draw.Scale(5);
		int lOffset = 0, rOffset = 0, bOffset = 0, tOffset = 0;
		I::MatSystemSurface->DrawSetAlphaMultiplier(tCache.m_flAlpha);

		if (tCache.m_bBox)
			H::Draw.LineRectOutline(x, y, w, h, tCache.m_tColor, { 0, 0, 0, 255 });
		for (auto& [iMode, flPercent, tColor, tOverfill, bAdjust] : tCache.m_vBars)
		{
			auto fDrawBar = [&](int x, int y, int w, int h, EAlign eAlign = ALIGN_LEFT)
			{
				if (flPercent > 1.f)
				{
					H::Draw.FillRectPercent(x, y, w, h, 1.f, tColor, { 0, 0, 0, 255 }, eAlign, bAdjust);
					H::Draw.FillRectPercent(x, y, w, h, flPercent - 1.f, tOverfill, { 0, 0, 0, 0 }, eAlign, bAdjust);
				}
				else
					H::Draw.FillRectPercent(x, y, w, h, flPercent, tColor, { 0, 0, 0, 255 }, eAlign, bAdjust);
			};

			int iSpace = H::Draw.Scale(4);
			int iThickness = H::Draw.Scale(2, Scale_Round);
			switch (iMode)
			{
			case ALIGN_LEFT:
				fDrawBar(x - iSpace - iThickness - lOffset, y, iThickness, h, ALIGN_BOTTOM);
				lOffset += iSpace + iThickness;
				break;
			case ALIGN_BOTTOM:
				fDrawBar(x, y + h + iSpace + bOffset, w, iThickness);
				bOffset += iSpace + iThickness;
				break;
			}
		}

		for (auto& [iMode, sText, tColor, tOutline] : tCache.m_vText)
		{
			switch (iMode)
			{
			case ALIGN_TOP:
				H::Draw.StringOutlined(fFont, m, t - tOffset, tColor, tOutline, ALIGN_BOTTOM, sText.c_str());
				tOffset += nTall;
				break;
			case ALIGN_BOTTOM:
				H::Draw.StringOutlined(fFont, m, b + bOffset, tColor, tOutline, ALIGN_TOP, sText.c_str());
				bOffset += nTall;
				break;
			case ALIGN_LEFT:
				H::Draw.StringOutlined(fFont, l - lOffset, y - H::Draw.Scale(2) + h - h * std::min(tCache.m_flHealth, 1.f), tColor, tOutline, ALIGN_TOPRIGHT, sText.c_str());
				break;
			case ALIGN_TOPRIGHT:
				H::Draw.StringOutlined(fFont, r, y - H::Draw.Scale(2) + rOffset, tColor, tOutline, ALIGN_TOPLEFT, sText.c_str());
				rOffset += nTall;
				break;
			case ALIGN_BOTTOMRIGHT:
				H::Draw.StringOutlined(fFont, r, y + h, tColor, tOutline, ALIGN_TOPLEFT, sText.c_str());
				break;
			}
		}
	}

	I::MatSystemSurface->DrawSetAlphaMultiplier(1.f);
}

void CESP::DrawWorld()
{
	if (m_mEntityCache.empty())
		return;

	const auto& fFont = H::Fonts.GetFont(FONT_ESP);
	const int nTall = fFont.m_nTall + H::Draw.Scale(2);
	for (auto& [pEntity, tCache] : m_mEntityCache)
	{
		float x, y, w, h;
		if (!GetDrawBounds(pEntity, x, y, w, h))
			continue;

		int l = x - H::Draw.Scale(6), r = x + w + H::Draw.Scale(6), m = x + w / 2;
		int t = y - H::Draw.Scale(5), b = y + h + H::Draw.Scale(5);
		int lOffset = 0, rOffset = 0, bOffset = 0, tOffset = 0;
		I::MatSystemSurface->DrawSetAlphaMultiplier(tCache.m_flAlpha);

		if (tCache.m_bBox)
			H::Draw.LineRectOutline(x, y, w, h, tCache.m_tColor, { 0, 0, 0, 255 });


		for (auto& [iMode, sText, tColor, tOutline] : tCache.m_vText)
		{
			switch (iMode)
			{
			case ALIGN_TOP:
				H::Draw.StringOutlined(fFont, m, t - tOffset, tColor, tOutline, ALIGN_BOTTOM, sText.c_str());
				tOffset += nTall;
				break;
			case ALIGN_BOTTOM:
				H::Draw.StringOutlined(fFont, m, b + bOffset, tColor, tOutline, ALIGN_TOP, sText.c_str());
				bOffset += nTall;
				break;
			case ALIGN_TOPRIGHT:
				H::Draw.StringOutlined(fFont, r, y - H::Draw.Scale(2) + rOffset, tColor, tOutline, ALIGN_TOPLEFT, sText.c_str());
				rOffset += nTall;
				break;
			}
		}
	}

	I::MatSystemSurface->DrawSetAlphaMultiplier(1.f);
}

bool CESP::GetDrawBounds(CBaseEntity* pEntity, float& x, float& y, float& w, float& h)
{
	Math::MatrixInitialize(s_mTransform, pEntity->GetAbsOrigin(), false);

	float flLeft, flRight, flTop, flBottom;
	if (!SDK::IsOnScreen(pEntity, s_mTransform, &flLeft, &flRight, &flTop, &flBottom, true))
		return false;

	x = flLeft;
	y = flBottom;
	w = flRight - flLeft;
	h = flTop - flBottom;

	switch (pEntity->GetClassID())
	{
	case ETFClassID::CTFPlayer:
	case ETFClassID::CObjectSentrygun:
	case ETFClassID::CObjectDispenser:
	case ETFClassID::CObjectTeleporter:
		x += w * 0.125f;
		w *= 0.75f;
	}

	return !(x > H::Draw.m_nScreenW || x + w < 0 || y > H::Draw.m_nScreenH || y + h < 0);
}

void CESP::DrawBones(CTFPlayer* pPlayer, matrix3x4* aBones, std::span<const int> vBones, Color_t tColor)
{
	if (vBones.empty())
		return;

	// Each segment's start point is the previous segment's end point; carry the
	// resolved center + its W2S result forward so every joint is transformed and
	// projected exactly once per chain instead of twice.
	Vec3 vPrev = pPlayer->GetHitboxCenter(aBones, vBones[0]);
	Vec3 vScreenPrev;
	bool bPrevOnScreen = SDK::W2S(vPrev, vScreenPrev);

	for (size_t n = 1; n < vBones.size(); n++)
	{
		Vec3 vCur = pPlayer->GetHitboxCenter(aBones, vBones[n]);
		Vec3 vScreenCur;
		bool bCurOnScreen = SDK::W2S(vCur, vScreenCur);

		if (bCurOnScreen && bPrevOnScreen)
			H::Draw.Line(vScreenCur.x, vScreenCur.y, vScreenPrev.x, vScreenPrev.y, tColor);

		vScreenPrev = vScreenCur;
		bPrevOnScreen = bCurOnScreen;
	}
}