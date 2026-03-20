#include <iostream>
#include <string>


#include <cmath>
#include <cstdint>

enum Recommendations {
	TEMP_UP, TEMP_DOWN,
	HUM_UP, HUM_DOWN,
	OPTIMAL, THIS_COULD_GET_UGLY
};

std::string strRecommends[] = { "▲ Temperatura.", "▼ Temperatura.", "▲ Humedad.", "▼ Humedad.", "Óptimo.", "Fuera de Rango" };

float g_fVPDMinMaxRanges[2] = { 0.0f, 6.27f };

float g_fEnvironmentTemperature = 36.7f;
float g_fEnvironmentHumidity = 69.3f;
//////////////////////////////////////////////
float g_fTemperatureRanges[2] = { 18.0f, 26.0f };
float g_fHumidityRanges[2] = { 50.0f, 70.0f };
float g_fVPDRanges[2] = { 0.6f, 1.0f };
uint8_t g_nVPDCorrectionPriority = 0;	// 0 = humidity | 1 = temperature
//////////////////////////////////////////////

float CalculateVPD(float fTemperature, float fHumidity) {
	return ((6.112f * expf((17.67f * fTemperature) / (243.5f + fTemperature))) * (1.0f - fHumidity / 100.0f)) / 10.0f;
}

Recommendations GetVPDRecommendation() {
	struct Recommendation {
		Recommendations Recommendation;
		float Score;
		uint8_t Priority;
	};

	Recommendation pRecommendations[4];	// Only needs hum up/down & temp up/down
	uint8_t nCount = 0;

	if (g_fEnvironmentTemperature < g_fTemperatureRanges[0])
		pRecommendations[nCount++] = { TEMP_UP, 0.0f, 1 };
	else if (g_fEnvironmentTemperature > g_fTemperatureRanges[1])
		pRecommendations[nCount++] = { TEMP_DOWN, 0.0f, 1 };

	if (g_fEnvironmentHumidity < g_fHumidityRanges[0])
		pRecommendations[nCount++] = { HUM_UP, 0.0f, 0 };
	else if (g_fEnvironmentHumidity > g_fHumidityRanges[1])
		pRecommendations[nCount++] = { HUM_DOWN, 0.0f, 0 };

	float fCurrentVPD = CalculateVPD(g_fEnvironmentTemperature, g_fEnvironmentHumidity);

	if (nCount == 0) {
		if (fCurrentVPD >= g_fVPDRanges[0] && fCurrentVPD <= g_fVPDRanges[1])
			return OPTIMAL;
	} else {
		for (uint8_t i = 0; i < nCount - 1; i++) {
			for (uint8_t j = i + 1; j < nCount; j++) {
				if (abs(pRecommendations[i].Priority - g_nVPDCorrectionPriority) > abs(pRecommendations[j].Priority - g_nVPDCorrectionPriority))
					std::swap(pRecommendations[i], pRecommendations[j]);
			}
		}

		return pRecommendations[0].Recommendation;
	}

	float fIdealVPD = (g_fVPDRanges[0] + g_fVPDRanges[1]) / 2.0f;
	float fDeltaVPD = std::max(1.0f, fabsf(fCurrentVPD - fIdealVPD));

	struct Option {
		float Temperature;
		float Humidity;
		bool InRange;
		uint8_t Priority;
		Recommendations Recommendation;
	};

	Option pOptions[4] = {
		{ g_fEnvironmentTemperature + fDeltaVPD, g_fEnvironmentHumidity, g_fEnvironmentTemperature + fDeltaVPD <= g_fTemperatureRanges[1], 1, TEMP_UP },
		{ g_fEnvironmentTemperature - fDeltaVPD, g_fEnvironmentHumidity, g_fEnvironmentTemperature - fDeltaVPD >= g_fTemperatureRanges[0], 1, TEMP_DOWN },
		{ g_fEnvironmentTemperature, g_fEnvironmentHumidity + fDeltaVPD, g_fEnvironmentHumidity + fDeltaVPD <= g_fHumidityRanges[1], 0, HUM_UP },
		{ g_fEnvironmentTemperature, g_fEnvironmentHumidity - fDeltaVPD, g_fEnvironmentHumidity - fDeltaVPD >= g_fHumidityRanges[0], 0, HUM_DOWN }
	};

	nCount = 0;
	for (uint8_t i = 0; i < 4; i++) {
		if (!pOptions[i].InRange)
			continue;

		float fVPD = CalculateVPD(pOptions[i].Temperature, pOptions[i].Humidity);

		if (fVPD >= g_fVPDMinMaxRanges[0] && fVPD <= g_fVPDMinMaxRanges[1])
			pRecommendations[nCount++] = { pOptions[i].Recommendation, fVPD, pOptions[i].Priority };
	}

	for (uint8_t i = 0; i < nCount - 1; i++) {
		for (uint8_t j = i + 1; j < nCount; j++) {
			float fA = fabsf(pRecommendations[i].Score - fIdealVPD) - (pRecommendations[i].Priority == g_nVPDCorrectionPriority ? 0.5f : 0.0f);
			float fB = fabsf(pRecommendations[j].Score - fIdealVPD) - (pRecommendations[j].Priority == g_nVPDCorrectionPriority ? 0.5f : 0.0f);

			if (fA > fB)
				std::swap(pRecommendations[i], pRecommendations[j]);
		}
	}

	if (nCount > 0)
		return pRecommendations[0].Recommendation;

	return THIS_COULD_GET_UGLY;
}

int main()
{
	Recommendations r= GetVPDRecommendation();
	std::cout<<"Recomendation: "<<(strRecommends[r]);

	return 0;
}