#include "Alerts.h"

#include "../../../SDK/Helpers/Fonts/Fonts.h"

void CAlerts::EnsureFont()
{
	const auto& sName = Vars::Visuals::Alerts::FontName.Value;
	const int iSize = H::Draw.Scale(Vars::Visuals::Alerts::FontSize.Value, Scale_Round);
	const bool bBold = Vars::Visuals::Alerts::FontBold.Value;

	if (m_dwFont && sName == m_sFontName && iSize == m_iFontSize && bBold == m_bFontBold)
		return;

	if (!m_dwFont)
		m_dwFont = I::MatSystemSurface->CreateFont();
	if (m_dwFont)
		I::MatSystemSurface->SetFontGlyphSet(m_dwFont, sName.c_str(), iSize, bBold ? 700 : 400, 0, 0, FONTFLAG_ANTIALIAS);

	m_sFontName = sName;
	m_iFontSize = iSize;
	m_bFontBold = bBold;
}

// True if any enemy sniper's eye-forward ray currently passes within Radius
// units of the local player (in front of them, i.e. actually pointed our way).
bool CAlerts::SniperSightline(CTFPlayer* pLocal)
{
	const float flRadius = Vars::Visuals::Alerts::SniperSightlineRadius.Value;
	const Vec3 vMyPos = pLocal->GetShootPos();

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer->IsAlive() || pPlayer->IsDormant())
			continue;

		auto pWeapon = pPlayer->m_hActiveWeapon()->As<CTFWeaponBase>();
		if (!pWeapon)
			continue;

		const int iWeaponID = pWeapon->GetWeaponID();
		if (iWeaponID != TF_WEAPON_SNIPERRIFLE && iWeaponID != TF_WEAPON_SNIPERRIFLE_DECAP && iWeaponID != TF_WEAPON_SNIPERRIFLE_CLASSIC)
			continue;

		const Vec3 vEye = pPlayer->GetShootPos();
		Vec3 vForward = {};
		Math::AngleVectors(H::Entities.GetEyeAngles(pPlayer->entindex()), &vForward);

		const float flForwardDist = (vMyPos - vEye).Dot(vForward);
		if (flForwardDist <= 0.f) // sightline points away from us
			continue;

		if (vMyPos.DistTo(vEye + vForward * flForwardDist) > flRadius)
			continue;

		if (!SDK::VisPos(pLocal, pPlayer, vMyPos, vEye))
			continue;

		return true;
	}
	return false;
}

// True if any enemy is within Distance units, optionally gated on line of sight.
bool CAlerts::EnemyNear(CTFPlayer* pLocal)
{
	const float flDistance = Vars::Visuals::Alerts::EnemyNearDistance.Value;
	const bool bLOSOnly = Vars::Visuals::Alerts::EnemyNearLineOfSight.Value;
	const Vec3 vMyEye = pLocal->GetShootPos();

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer->IsAlive() || pPlayer->IsDormant())
			continue;

		if (pLocal->m_vecOrigin().DistTo(pPlayer->m_vecOrigin()) > flDistance)
			continue;

		if (bLOSOnly && !SDK::VisPos(pLocal, pPlayer, vMyEye, pPlayer->GetShootPos()))
			continue;

		return true;
	}
	return false;
}

void CAlerts::Draw(CTFPlayer* pLocal)
{
	if (!Vars::Visuals::Alerts::Enabled.Value || !I::EngineClient->IsInGame() || !pLocal->IsAlive())
		return;

	const std::string* aActive[2] = {};
	int iCount = 0;

	if (Vars::Visuals::Alerts::SniperSightlineEnabled.Value && SniperSightline(pLocal))
		aActive[iCount++] = &Vars::Visuals::Alerts::SniperSightlineText.Value;
	if (Vars::Visuals::Alerts::EnemyNearEnabled.Value && EnemyNear(pLocal))
		aActive[iCount++] = &Vars::Visuals::Alerts::EnemyNearText.Value;

	if (!iCount)
		return;

	EnsureFont();
	const Font_t tFont = { m_sFontName.c_str(), m_iFontSize, FONTFLAG_ANTIALIAS, m_bFontBold ? 700 : 400, m_dwFont };

	const int x = Vars::Visuals::Alerts::Display.Value.x;
	int y = Vars::Visuals::Alerts::Display.Value.y;
	const int iLineHeight = m_iFontSize + H::Draw.Scale(Vars::Visuals::Alerts::Spacing.Value, Scale_Round);

	const auto& tColor = Vars::Visuals::Alerts::Color.Value;
	const auto& tOutlineColor = Vars::Visuals::Alerts::OutlineColor.Value;
	const bool bOutline = Vars::Visuals::Alerts::Outline.Value;

	for (int i = 0; i < iCount; i++)
	{
		if (aActive[i]->empty())
			continue;

		if (bOutline)
			H::Draw.StringOutlined(tFont, x, y, tColor, tOutlineColor, ALIGN_TOP, aActive[i]->c_str());
		else
			H::Draw.String(tFont, x, y, tColor, ALIGN_TOP, aActive[i]->c_str());

		y += iLineHeight;
	}
}
