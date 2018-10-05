// MemoryFlipGameSDL2.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include <SDL.h>
#include <SDL_image.h>
#include <iostream> // for debug
#include <memory>
#include <string>
#include <filesystem>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>

// Important Note: 
// The unique id needs to be stored with the src rectangle, NOT the dst rectangle.
// The reason for this is not intuitive at all and may induce a headache if you try to work it out visually.
// However, I feel it is important to attempt to put it in writing because otherwise I may forget.
// And future me, or anyone possibly reading this in the future, may make the same mistake I did initially.

// In a nutshell, it boils down to two important facts:

// 1) The dst coordinates don't change when the vector containing them gets shuffled.
// All that changes is their sorted order in the list.

// 2) The src coordinates are duplicated, but the dst coordinates are not. 
// They are duplicated because we need two copies of each piece of the src image rendered, for the memory puzzle design.

// What this means is:
// If dst coordinates are shuffled and are contained in a struct with the unique id and state (as I originally did it)
// what happens is the syncing of the src coordinates and dst coordinates is changed, 
// creating the appearance of random display order.
// However, this is a trap to lull you into a false sense of security in having created a working game.
// The dst coordinates don't change, all that changes is the image that is displayed at their location.
// When you click on the location, you read whatever id is tied to those coordinates.
// Meaning, you get the appearance of a shuffled board with underlying ids that haven't actually been shuffled.
// So prior to shuffling, you click on a picture of a frog and a frog and it's a match and makes sense.
// After shuffling, you click on a picture of a frog and a snake, and it's a match and makes no sense.
// Because the frogs never moved relative to their ids. So it thinks a frog and a snake are a match because they are.


// Why it works to store it with src coordinates:
// With the unique id and state being stored with the src coordinates, the mouseclick code looks something like this:
// if (mouseWithinRectBound(sdlEvent.button, dstCoords[i]) && 
// puzzlePiecesAll[i].visState == puzzlePiece::VisState::HIDDEN)

// With dstCoords having been shuffled, if we click on the first element of dstCoords,
// we're also getting the state that is tied to the src image piece and the unique id.
// This means that appearance/id/state are all linked up. 

// So, for example, we click on point x=40, y=80. 
// The related element for src is: id=1234567890, state=HIDDEN, srcCoordinates x=0, y=0

// The image is displayed, when two are displayed, their ids are checked duplication.
// And if they are the same id, it's a match.

// Very, very simple. 
// I need to go lie down.



std::random_device rd;
std::mt19937 mt(rd());

const int puzzlePieceSize = 40; // 40x40
const int puzzlePiecesTotal = 100;

std::vector<SDL_Rect> srcCoords(puzzlePiecesTotal);
std::vector<SDL_Rect> dstCoords(puzzlePiecesTotal);

struct puzzlePiece
{
	SDL_Rect srcRect;
	enum class VisState { HIDDEN, FLIPPED, SOLVED };
	VisState visState = VisState::HIDDEN;
	std::string id;
};
std::vector<puzzlePiece> puzzlePiecesAll(puzzlePiecesTotal);
const int maxFlipped = 2; // The maximum number of "pieces" that can be in the flipped up state at the same time.
int flippedCount = 0;
std::vector<int> flippedIndices(2);
int flipTimer = 0;


const int fpsCap = 60;
const int fpsDelay = 1000 / fpsCap;
Uint32 fpsTimerStart;
int fpsTimerElapsed;

struct sdlDestructorWindow
{
	void operator()(SDL_Window *window) const
	{
		SDL_DestroyWindow(window);
		SDL_Log("SDL_Window deleted");
	}
};

struct sdlDestructorRenderer
{
	void operator()(SDL_Renderer *renderer) const
	{
		SDL_DestroyRenderer(renderer);
		SDL_Log("SDL_Renderer deleted");
	}
};

struct sdlDestructorTexture
{
	void operator()(SDL_Texture *texture) const
	{
		SDL_DestroyTexture(texture);
		SDL_Log("SDL_Texture deleted");
	}
};

std::unique_ptr<SDL_Window, sdlDestructorWindow> window;
std::unique_ptr<SDL_Renderer, sdlDestructorRenderer> renderer;
std::vector<std::unique_ptr<SDL_Texture, sdlDestructorTexture>> puzzleTextures;
std::unique_ptr<SDL_Texture, sdlDestructorTexture> pieceHiddenTex;
std::unique_ptr<SDL_Texture, sdlDestructorTexture> flippedOutlineTex;

enum class ProgramState { STARTUP, PLAY, TRANSITION, SHUTDOWN };
ProgramState programState = ProgramState::STARTUP;

void programStartup();
void programShutdown();
void eventPoll();
void renderUpdate();
void shufflePuzzlePieces();
bool mouseWithinRectBound(const SDL_MouseButtonEvent &btn, const SDL_Rect &rect);
bool puzzleSolved();

int main(int argc, char *argv[])
{
	while (programState != ProgramState::SHUTDOWN)
	{
		switch (programState)
		{
		case (ProgramState::STARTUP):
			programStartup();
			programState = ProgramState::PLAY;
			break;
		case (ProgramState::PLAY):
			fpsTimerStart = SDL_GetTicks();
			eventPoll();
			renderUpdate();
			fpsTimerElapsed = SDL_GetTicks() - fpsTimerStart;
			if (fpsDelay > fpsTimerElapsed)
			{
				SDL_Delay(fpsDelay - fpsTimerElapsed);
			}
			break;
		case (ProgramState::TRANSITION):
			break;
		}
	}

	programShutdown();

	return 0;
}

void programStartup()
{
	SDL_Init(SDL_INIT_EVERYTHING);

	window.reset(SDL_CreateWindow("Memory Flip Game", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 600, 600, false));
	renderer.reset(SDL_CreateRenderer(window.get(), -1, 0));
	SDL_SetRenderDrawColor(renderer.get(), 242, 242, 242, 255);

	// Get texture for hidden state pieces.
	{
		SDL_Surface *tmpSurface;
		tmpSurface = IMG_Load("textures/hiddenStateTexture.png");
		pieceHiddenTex.reset(SDL_CreateTextureFromSurface(renderer.get(), tmpSurface));
		SDL_FreeSurface(tmpSurface);

		tmpSurface = IMG_Load("textures/flippedStateOutlineTexture.png");
		flippedOutlineTex.reset(SDL_CreateTextureFromSurface(renderer.get(), tmpSurface));
		SDL_FreeSurface(tmpSurface);
	}

	// Store puzzle image textures in vector of unique pointers.
	{
		std::string puzzlesPath = "puzzles/";
		auto dirIter = std::experimental::filesystem::directory_iterator(puzzlesPath);
		for (auto& file : dirIter)
		{
			if (file.path().filename().string().find(".png") != std::string::npos)
			{
				SDL_Surface *tmpSurface;
				tmpSurface = IMG_Load(file.path().string().c_str());
				std::unique_ptr<SDL_Texture, sdlDestructorTexture> puzzleTex;
				puzzleTex.reset(SDL_CreateTextureFromSurface(renderer.get(), tmpSurface));
				puzzleTextures.push_back(std::move(puzzleTex));
				SDL_FreeSurface(tmpSurface);
			}
		}
	}

	// Set src coords.
	{
		int xOffset = 0;
		int yOffset = 0;
		int rowCount = 0;
		const int xRowLen = (sqrt(puzzlePiecesTotal) / 2) - 1;
		const int sizeHalf = puzzlePiecesTotal / 2;
		{
			for (int rectI = 0; rectI < sizeHalf; rectI++)
			{
				puzzlePiecesAll[rectI].srcRect.w = puzzlePieceSize;
				puzzlePiecesAll[rectI].srcRect.h = puzzlePieceSize;
				puzzlePiecesAll[rectI].srcRect.x = xOffset;
				puzzlePiecesAll[rectI].srcRect.y = yOffset;

				std::string str = "";
				std::uniform_int_distribution<int> dist(1, 9);
				for (int i = 0; i < 10; i++)
				{
					str += std::to_string(dist(mt));
				}
				puzzlePiecesAll[rectI].id = str;

				if (rowCount >= xRowLen)
				{
					rowCount = 0;
					xOffset = 0;
					yOffset += puzzlePieceSize;
				}
				else
				{
					xOffset += puzzlePieceSize;
					rowCount++;
				}
			}
		}

		{
			std::copy(puzzlePiecesAll.begin(), puzzlePiecesAll.begin() + sizeHalf, puzzlePiecesAll.begin() + sizeHalf);
		}
	}

	// Set dst coords.
	{
		const int xBoardOffset = 75;
		const int yBoardOffset = 40;
		const int betweenPiecesOffset = 5;
		int xOffset = 0;
		int yOffset = 0;
		int rowCount = 0;
		const int xRowLen = sqrt(puzzlePiecesTotal) - 1;
		for (auto& rect : dstCoords)
		{
			rect.w = puzzlePieceSize;
			rect.h = puzzlePieceSize;
			rect.x = xOffset + xBoardOffset;
			rect.y = yOffset + yBoardOffset;

			if (rowCount >= xRowLen)
			{
				rowCount = 0;
				xOffset = 0;
				yOffset += puzzlePieceSize + betweenPiecesOffset;
			}
			else
			{
				xOffset += puzzlePieceSize + betweenPiecesOffset;
				rowCount++;
			}
		}
	}

	shufflePuzzlePieces();
}

void programShutdown()
{
	SDL_Quit();
}

void eventPoll()
{
	SDL_Event sdlEvent;
	SDL_PollEvent(&sdlEvent);

	switch (sdlEvent.type)
	{
	case SDL_QUIT:
		programState = ProgramState::SHUTDOWN;
		break;
	case SDL_MOUSEBUTTONDOWN:
		if (sdlEvent.button.button == SDL_BUTTON_LEFT)
		{
			for (int i = 0; i < puzzlePiecesTotal; i++)
			{
				if (mouseWithinRectBound(sdlEvent.button, dstCoords[i]) && 
					puzzlePiecesAll[i].visState == puzzlePiece::VisState::HIDDEN)
				{
					if (flippedCount < maxFlipped)
					{
						if (flippedCount == 0)
						{
							flippedIndices[0] = i;
						}
						else if (flippedCount == 1)
						{
							flippedIndices[1] = i;
						}
						puzzlePiecesAll[i].visState = puzzlePiece::VisState::FLIPPED;
						flippedCount++;
						break;
					}
					break;
				}
			}
		}
		break;
	}

	if (flippedCount >= maxFlipped)
	{		
		flipTimer++;
		if (flipTimer > 40)
		{
			if (puzzlePiecesAll[flippedIndices[0]].id == puzzlePiecesAll[flippedIndices[1]].id)
			{
				puzzlePiecesAll[flippedIndices[0]].visState = puzzlePiece::VisState::SOLVED;
				puzzlePiecesAll[flippedIndices[1]].visState = puzzlePiece::VisState::SOLVED;
				if (puzzleSolved())
				{
					programState = ProgramState::TRANSITION;
				}
			}
			else
			{
				puzzlePiecesAll[flippedIndices[0]].visState = puzzlePiece::VisState::HIDDEN;
				puzzlePiecesAll[flippedIndices[1]].visState = puzzlePiece::VisState::HIDDEN;
			}
			flippedCount = 0;
			flipTimer = 0;
		}
	}
}

void renderUpdate()
{
	SDL_RenderClear(renderer.get());
	for (int rectI = 0; rectI < puzzlePiecesTotal; rectI++)
	{
		if (puzzlePiecesAll[rectI].visState == puzzlePiece::VisState::HIDDEN)
		{
			SDL_RenderCopy(renderer.get(), pieceHiddenTex.get(), NULL, &dstCoords[rectI]);
		}
		else if (puzzlePiecesAll[rectI].visState == puzzlePiece::VisState::FLIPPED)
		{
			SDL_RenderCopy(renderer.get(), puzzleTextures[0].get(), &puzzlePiecesAll[rectI].srcRect, &dstCoords[rectI]);
			SDL_RenderCopy(renderer.get(), flippedOutlineTex.get(), NULL, &dstCoords[rectI]);
		}
	}
	SDL_RenderPresent(renderer.get());
}

void shufflePuzzlePieces()
{
	int seed = std::chrono::system_clock::now().time_since_epoch().count();
	shuffle(puzzlePiecesAll.begin(), puzzlePiecesAll.end(), std::default_random_engine(seed));
}

bool mouseWithinRectBound(const SDL_MouseButtonEvent &btn, const SDL_Rect &rect)
{
	if (btn.x >= rect.x &&
		btn.x <= rect.x + rect.w &&
		btn.y >= rect.y &&
		btn.y <= rect.y + rect.h)
	{
		return true;
	}
	return false;
}

bool puzzleSolved()
{
	for (auto obj : puzzlePiecesAll)
	{
		if (obj.visState != puzzlePiece::VisState::SOLVED)
		{
			return false;
		}
	}
	return true;
}