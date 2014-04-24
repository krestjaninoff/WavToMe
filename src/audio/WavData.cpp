#include <iostream>
#include <fstream>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <limits>
#include <limits.h>
#include <cassert>
#include <vector>
#include <math.h>
#include "audio.h"
#include "WavData.h"
#include "Frame.h"
#include "Word.h"

using namespace std;

namespace wtm {
namespace audio {

/**
 * Read Wav data from a file
 */
WavDataPtr WavData::readFromFile(const std::string& file) {
	WavHeader wavHeader;

	// Open file
	std::fstream fs;
	fs.open(file.c_str(), std::ios::in | std::ios::binary);

	if (!fs.good()) {
		fprintf(stderr, "Can't open the wave file\n");
		exit(EXIT_FAILURE);
	}

	// Read header
	fs.read((char*)(&wavHeader), sizeof(WavHeader));
	checkHeader(wavHeader);

	// Read raw data
	WavDataPtr wavData(new WavData(wavHeader));
	readRawData(fs, wavHeader, *wavData);
	fs.close();

	return wavData;
}

void WavData::init() {

	this->divideIntoFrames();
	this->divideIntoWords();
}

/**
 * Checks a set of restrictions
 */
void WavData::checkHeader(const WavHeader& wavHeader) {

	if (0 != strncmp(wavHeader.riff, "RIFF", sizeof(wavHeader.riff))
			|| 0 != strncmp(wavHeader.wave, "WAVE", sizeof(wavHeader.wave))) {
		fprintf(stderr, "Invalid RIFF/WAVE format\n");
		exit(EXIT_FAILURE);
	}

	if (1 != wavHeader.audioFormat) {
		fprintf(stderr, "Invalid WAV format: only PCM audio format is supported\n");
		exit(EXIT_FAILURE);
	}

	if (wavHeader.numOfChan > 2) {
		fprintf(stderr, "Invalid WAV format: only 1 or 2 channels audio is supported\n");
		exit(EXIT_FAILURE);
	}

	unsigned long bitsPerChannel = wavHeader.bitsPerSample / wavHeader.numOfChan;
	if (8 != bitsPerChannel && 16 != bitsPerChannel) {
		fprintf(stderr, "Invalid WAV format: only 8 and 16-bit per channel is supported\n");
		exit(EXIT_FAILURE);
	}

	if (wavHeader.subchunk2Size > LONG_MAX) {
		fprintf(stderr, "File too big\n");
		exit(EXIT_FAILURE);
	}
}

void WavData::readRawData(std::fstream& fs, const WavHeader& wavHeader, WavData& wavFile) {
	raw_t value, minValue = 0, maxValue = 0;
	uint8_t value8, valueLeft8, valueRight8;
	int16_t value16, valueLeft16, valueRight16;

	length_t bytesPerSample = static_cast<uint32_t>(wavHeader.bitsPerSample / 8);
	unsigned long numberOfSamplesXChannels = wavHeader.subchunk2Size /
			(wavHeader.numOfChan * bytesPerSample);

	unsigned long sampleNumber = 0;
	for (; sampleNumber < numberOfSamplesXChannels && !fs.eof(); sampleNumber++) {

		if (8 == wavHeader.bitsPerSample) {
			if (1 == wavHeader.numOfChan) {
				fs.read((char*)(&value8), sizeof(uint8_t));
				value = static_cast<raw_t>(value8);

			} else {
				fs.read((char*)(&valueLeft8), sizeof(uint8_t));
				fs.read((char*)(&valueRight8), sizeof(uint8_t));
				value = static_cast<raw_t>((abs(valueLeft8) + abs(valueRight8)) / 2);
			}
		} else {
			if (1 == wavHeader.numOfChan) {
				fs.read((char*)(&value16), sizeof(int16_t));
				value = static_cast<raw_t>(value16);

			} else {
				fs.read((char*)(&valueLeft16), sizeof(int16_t));
				fs.read((char*)(&valueRight16), sizeof(int16_t));
				value = static_cast<raw_t>((abs(valueLeft16) + abs(valueRight16)) / 2);
			}
		}

		if (maxValue < value) {
			maxValue = value;
		}

		if (minValue > value) {
			minValue = value;
		}

		wavFile.rawData->push_back(value);
	}
	assert(sampleNumber > 0);

	// Update values
	wavFile.setMinVal(minValue);
	wavFile.setMaxVal(maxValue);
	wavFile.setNumberOfSamples(sampleNumber);

	length_t bytesPerFrame = static_cast<length_t>(wavHeader.bytesPerSec * FRAME_LENGTH / 1000.0);
	wavFile.samplesPerFrame = static_cast<length_t>(bytesPerFrame / bytesPerSample);
	assert(wavFile.samplesPerFrame > 0);
}

void WavData::divideIntoFrames() {
	assert(header.subchunk2Size > 0);

	unsigned int samplesPerNonOverlap =
		static_cast<unsigned int>(samplesPerFrame * (1 - FRAME_OVERLAP));
	unsigned int framesCount =
		(header.subchunk2Size / (header.bitsPerSample / 8)) / samplesPerNonOverlap;

	frames->reserve(framesCount);

	length_t indexBegin = 0, indexEnd = 0;
	for (length_t i = 0, size = rawData->size(); i < framesCount; ++i) {

		indexBegin = i * samplesPerNonOverlap;
		indexEnd = indexBegin + samplesPerFrame;
		if (indexEnd < size) {

			Frame* frame = new Frame(i, *rawData, indexBegin, indexEnd);
			frames->push_back(frame);
		} else {
			break;
		}
	}
}

void WavData::divideIntoWords() {
	assert(frames->size() > 10);

	double maMin = 0;
	double maAvg = 0;
	double maMax = 0;
	double ma;

	// Let's use Moving Average value to avoid spikes
	unsigned short maShift = MOVING_AVERAGE_SIZE / 2;
	maAvg = maMin = frames->at(0)->calcRms();
	length_t iFrame;
	for (iFrame = maShift; iFrame < frames->size() - maShift; ++iFrame) {

		ma = 0;
		for (unsigned short iMa = iFrame - maShift; iMa <= iFrame + maShift; iMa++) {
			ma += frames->at(iMa)->calcRms();
		}
		ma /= MOVING_AVERAGE_SIZE;
		frames->at(iFrame)->setMaRms(ma);

		if (maMin > ma) {
			maMin = ma;
		}
		if (ma > maMax) {
			maMax = ma;
		}

		maAvg += ma;
	}
	maAvg /= iFrame;
	this->maRmsMax = maMax;

	// A little hack to calculate bound values
	for (length_t iFrame = 0; iFrame < maShift; ++iFrame) {
		frames->at(iFrame)->setMaRms(frames->at(iFrame)->calcRms());
		frames->at(frames->size() - 1 - iFrame)->setMaRms(
				frames->at(frames->size() - 1 - iFrame)->calcRms());
	}

	// Tries to guess the best threshold value
	double thresholdCandidate = getThresholdCandidate(maMin, maAvg, maMax);
	this->wordsThreshold = thresholdCandidate;

	// If max value greater than min value more then 50% then we have the "silence" threshold.
	// Otherwise, let's think that we have only one word.
	double threshold = 0;
	if (maMax * 0.5 > maMin) {
		threshold = thresholdCandidate;

		// Divide frames into words
		long firstFrameInCurrentWordNumber = -1;
		vector<Frame*>::const_iterator firstFrame;
		vector<Frame*>::const_iterator lastFrame;
		Word* lastWord = 0;
		for (vector<Frame*>::const_iterator frame = frames->begin();
				frame != frames->end(); ++frame) {

			// Got a sound
			if ((*frame)->getMaRms() > threshold) {

				if (-1 == firstFrameInCurrentWordNumber) {
					firstFrameInCurrentWordNumber = (*frame)->getNumber();
					DEBUG("Word started at frame %d", firstFrameInCurrentWordNumber);
				}

			// Got silence
			} else {
				if (firstFrameInCurrentWordNumber >= 0) {

					// Let's find distance between start of the current word and end of the previous word
					length_t distance = 0;
					if (0 != lastWord) {
						Frame* lastFrameInPreviousWord = lastWord->getFrames()->at(
								lastWord->getFrames()->size() - 1);
						distance = firstFrameInCurrentWordNumber - lastFrameInPreviousWord->getNumber();
					}

					// We have a new word
					if (0 == lastWord || distance >= WORDS_MIN_DISTANCE) {

						firstFrame = frames->begin() + firstFrameInCurrentWordNumber;
						lastFrame =	frames->begin() + (*frame)->getNumber();

						std::vector<Frame*>* extendedFrames = new std::vector<Frame*>(
								firstFrame, lastFrame);
						lastWord = new Word(extendedFrames);

						this->words->push_back(lastWord);
						DEBUG("Word finished at frame %d", (*frame)->getNumber());

					// We need to add the current word to the previous one
					} else if (0 != lastWord && distance < WORDS_MIN_DISTANCE) {
						firstFrame = frames->begin() + (*lastWord->getFrames()->begin())->getNumber();
						lastFrame =	frames->begin() + (*frame)->getNumber();

						std::vector<Frame*>* extendedFrames = new std::vector<Frame*>(
								firstFrame, lastFrame);
						lastWord = new Word(extendedFrames);

						this->words->pop_back();
						this->words->push_back(lastWord);

						DEBUG("Word finished at frame %d and added to previous one", frame - frames->begin());
					}

					firstFrameInCurrentWordNumber = -1;
				}
			}
		}

		// Clean up short words
		for (vector<Word*>::iterator word = this->words->begin();
				word != this->words->end(); ++word) {
			if ((*word)->getFramesCount() < WORD_MIN_SIZE) {
				this->words->erase(word);
			}
		}


	// Seems we have only one word
	} else {
		this->words->push_back(new Word(this->frames));
	}
}

/**
 * Determination of silence threshold
 *
 * Method divides data into 3 clusters (using something like k-means algorithm).
 * The cluster center of "Min" cluster is used as a threshold candidate.
 */
double WavData::getThresholdCandidate(double maMin, double maAvg, double maMax) {
	short currIter = 0, maxIterCnt = 30;
	bool isCenterChanged = true;

	// Init clusters
	double minClusterCenter = maMin;
	double minClusterCenterNew = 0;
	std::vector<Frame*>* minCluster = new std::vector<Frame*>();

	// TODO May be maAvg will serve better?
	double avgClusterCenter = maMax / 2;
	double avgClusterCenterNew = 0;
	std::vector<Frame*>* avgCluster = new std::vector<Frame*>();

	double maxClusterCenter = maMax;
	double maxClusterCenterNew = 0;
	std::vector<Frame*>* maxCluster = new std::vector<Frame*>();

	double maRms;
	for (vector<Frame*>::const_iterator frame = frames->begin();
		frame != frames->end(); ++frame) {

		maRms = (*frame)->getMaRms();

		if (fabs(maRms - minClusterCenter) < fabs(maRms - avgClusterCenter)
				&& fabs(maRms - minClusterCenter) < fabs(maRms - maxClusterCenter)) {
			minCluster->push_back(*frame);

		} else if (fabs(maRms - avgClusterCenter) < fabs(maRms - minClusterCenter)
				&& fabs(maRms - avgClusterCenter) < fabs(maRms - maxClusterCenter)) {
			avgCluster->push_back(*frame);

		} else {
			maxCluster->push_back(*frame);
		}
	}

	// Iterate
	while (currIter < maxIterCnt && isCenterChanged) {

		DEBUG("Min center: %f, size: %d", minClusterCenter, minCluster->size());
		DEBUG("Avg center: %f, size: %d", avgClusterCenter, avgCluster->size());
		DEBUG("Max center: %f, size: %d", maxClusterCenter, maxCluster->size());
		DEBUG("_");

		// Calculates new cluster centers
		if (minCluster->size() > 0) {
			minClusterCenterNew = minCluster->at(0)->getMaRms();

			for (vector<Frame*>::const_iterator frame = minCluster->begin();
						frame != minCluster->end(); ++frame) {
				minClusterCenterNew += (*frame)->getMaRms();
			}
			minClusterCenterNew /= minCluster->size();

		} else {
			break;
		}

		if (avgCluster->size() > 0) {
			avgClusterCenterNew = avgCluster->at(0)->getMaRms();

			for (vector<Frame*>::const_iterator frame = avgCluster->begin();
						frame != avgCluster->end(); ++frame) {
				avgClusterCenterNew += (*frame)->getMaRms();
			}
			avgClusterCenterNew /= avgCluster->size();

		} else {
			break;
		}

		if (maxCluster->size() > 0) {
			maxClusterCenterNew = maxCluster->at(0)->getMaRms();

			for (vector<Frame*>::const_iterator frame = maxCluster->begin();
						frame != maxCluster->end(); ++frame) {
				maxClusterCenterNew += (*frame)->getMaRms();
			}
			maxClusterCenterNew /= maxCluster->size();

		} else {
			break;
		}

		// Check if clusters centers changed
		if (fabs(minClusterCenterNew - minClusterCenter) < numeric_limits<double>::epsilon()
				&& fabs(avgClusterCenterNew - avgClusterCenter) < numeric_limits<double>::epsilon()
				&& fabs(maxClusterCenterNew - maxClusterCenter) < numeric_limits<double>::epsilon()) {
			isCenterChanged = false;
			break;
		}

		// Update clusters centers
		minClusterCenter = minClusterCenterNew;
		avgClusterCenter = avgClusterCenterNew;
		maxClusterCenter = maxClusterCenterNew;

		// Rebuild clusters
		minCluster->clear();
		avgCluster->clear();
		maxCluster->clear();

		for (vector<Frame*>::const_iterator frame = frames->begin();
				frame != frames->end(); ++frame) {

			if (fabs((*frame)->getMaRms() - minClusterCenter) < fabs((*frame)->getMaRms() - avgClusterCenter)
					&& fabs((*frame)->getMaRms() - minClusterCenter) < fabs((*frame)->getMaRms() - maxClusterCenter)) {

				minCluster->push_back(*frame);

			} else if (fabs((*frame)->getMaRms() - avgClusterCenter) < fabs((*frame)->getMaRms() - minClusterCenter)
					&& fabs((*frame)->getMaRms() - avgClusterCenter) < fabs((*frame)->getMaRms() - maxClusterCenter)) {

				avgCluster->push_back(*frame);

			} else {
				maxCluster->push_back(*frame);
			}
		}

		currIter++;
	}

	double thresholdCandidate = minClusterCenter;
	DEBUG("Threshold candidate: %f", thresholdCandidate);

	delete minCluster;
	delete avgCluster;
	delete maxCluster;

	return thresholdCandidate;
}

void WavData::saveToFile(const std::string& file, const Word& word) const {

	// number of data bytes in the resulting wave file
	unsigned int samplesPerNonOverlap =
			static_cast<unsigned int>(samplesPerFrame * (1 - FRAME_OVERLAP));
	unsigned int waveSize = word.getFrames()->size() * samplesPerNonOverlap * sizeof(raw_t);

	// prepare a new header and write it to file stream
	WavHeader headerNew;
	strncpy(headerNew.riff, this->header.riff, 4);
	headerNew.chunkSize = waveSize + sizeof(WavHeader);
	strncpy(headerNew.wave, this->header.wave, 4);
	strncpy(headerNew.fmt, this->header.fmt, 4);
	headerNew.subchunk1Size = this->header.subchunk1Size;
	headerNew.audioFormat = this->header.audioFormat;
	headerNew.numOfChan = 1;
	headerNew.samplesPerSec = this->header.samplesPerSec;
	headerNew.bytesPerSec = this->header.samplesPerSec * sizeof(raw_t);
	headerNew.blockAlign = sizeof(raw_t);
	headerNew.bitsPerSample = sizeof(raw_t) * 8;
	strncpy(headerNew.data, this->header.data, 4);
	headerNew.subchunk2Size = waveSize;

	std::fstream fs;
	fs.open(file.c_str(), std::ios::out | std::ios::binary);
	fs.write((char*)&headerNew, sizeof(WavHeader));

	raw_t* data = new raw_t[waveSize / sizeof(raw_t)];

	int frameNumber = 0;
	for (vector<Frame*>::const_iterator frame = word.getFrames()->begin();
			frame != word.getFrames()->end(); ++frame) {

		for (length_t i = 0; i < samplesPerNonOverlap; i++) {
			data[frameNumber * samplesPerNonOverlap + i ] =
					this->rawData->at((*frame)->getStart() + i);

			DEBUG("Frame %d (%d): %d", frameNumber,
					(*frame)->getStart() + i, this->rawData->at((*frame)->getStart() + i));
		}

		frameNumber++;
	}

	fs.write((char*)data, waveSize);
	fs.close();
	delete [] data;
}

bool WavData::isPartOfWord(const Frame* frame) const {
	bool isPartOfWord = false;

	for (vector<Word*>::const_iterator word = this->words->begin();
			word != this->words->end(); ++word) {

		length_t left = (*(*word)->getFrames()->begin())->getNumber();
		length_t right = (*word)->getFrames()->at((*word)->getFrames()->size() - 1)->getNumber();

		if (left <= frame->getNumber() && frame->getNumber() <= right) {

			isPartOfWord = true;
			break;
		}
	}

	return isPartOfWord;
}

} // namespace audio
} // namespace wtm
