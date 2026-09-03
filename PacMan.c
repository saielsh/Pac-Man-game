#include<stdio.h>
#include<raylib.h>
#include<time.h>
#include<stdlib.h>
#include<math.h>

// ---- coin settings ----
#define CELL_SIZE 40          // spacing between coin grid points, keeps them uniform
#define COIN_SIZE 10          // size of each coin's collision box
#define MAX_COINS 200         // upper bound for how many coins we can ever have
#define COIN_RESPAWN_TIME 8.0 // seconds before an eaten coin comes back
#define COIN_SCORE_VALUE 10   // points earned per coin eaten

typedef struct {
    Rectangle rect;     // collision box used for eating + drawing position
    bool active;        // true = visible and can be eaten
    double respawnAt;   // GetTime() value when this coin should reappear
} Coin;

// ---- ghost roster settings ----
#define TUNNEL_ROW_Y 300

typedef struct {
    Rectangle rect;      // current position + size
    int textureIndex;    // which loaded ghost texture to draw with (wraps around if you add more ghosts than textures)
    bool active;         // false while this ghost is still waiting to spawn in
    double spawnDelay;   // seconds after ghost mode starts before this one enters
} Ghost;

#define PLAYER_COLLISION_MARGIN 4

#define GRID_CELL 20
#define GRID_COLS 40   // 800 / GRID_CELL
#define GRID_ROWS 30   // 600 / GRID_CELL

// ---- ghost spacing settings ----
#define GHOST_PATH_AVOID_RADIUS 1.5f   // in grid cells
#define GHOST_PATH_AVOID_PENALTY 6     // added to a candidate cell's score if another ghost is near it
#define GHOST_MIN_SEPARATION 34.0f     // in pixels, roughly one ghost's width

// ---- game state / scoring ----
typedef enum { STATE_PLAYING, STATE_GAME_OVER } GameState;
#define HIGH_SCORE_FILE "highscore.txt"

// flood-fills "distance to (startCol, startRow)" into dist[][] for every
// reachable open cell, -1 means unreachable / not yet visited.
void computeDistanceField(bool blocked[GRID_COLS][GRID_ROWS], int startCol, int startRow, int dist[GRID_COLS][GRID_ROWS]){
    for(int x = 0; x < GRID_COLS; x++){
        for(int y = 0; y < GRID_ROWS; y++){
            dist[x][y] = -1;
        }
    }

    if(startCol < 0 || startCol >= GRID_COLS || startRow < 0 || startRow >= GRID_ROWS){
        return;
    }
    if(blocked[startCol][startRow]){
        return; // target cell reads as a wall (shouldn't normally happen) - bail out safely
    }

    // plain array-based queue, sized so it can never overflow (every cell
    // can only be pushed once since we check dist == -1 before pushing)
    static int qx[GRID_COLS * GRID_ROWS];
    static int qy[GRID_COLS * GRID_ROWS];
    int head = 0, tail = 0;

    dist[startCol][startRow] = 0;
    qx[tail] = startCol; qy[tail] = startRow; tail++;

    int dx[4] = {1, -1, 0, 0};
    int dy[4] = {0, 0, 1, -1};

    while(head < tail){
        int cx = qx[head];
        int cy = qy[head];
        head++;

        for(int d = 0; d < 4; d++){
            int nx = cx + dx[d];
            int ny = cy + dy[d];
            if(nx < 0 || nx >= GRID_COLS || ny < 0 || ny >= GRID_ROWS) continue;
            if(blocked[nx][ny]) continue;
            if(dist[nx][ny] != -1) continue;

            dist[nx][ny] = dist[cx][cy] + 1;
            qx[tail] = nx; qy[tail] = ny; tail++;
        }
    }
}

void moveGhostAlongPath(Rectangle *ghost, int dist[GRID_COLS][GRID_ROWS], bool blocked[GRID_COLS][GRID_ROWS], float speed, Ghost *allGhosts, int ghostCount, int selfIndex){
    int col = (int)((ghost->x + ghost->width / 2) / GRID_CELL);
    int row = (int)((ghost->y + ghost->height / 2) / GRID_CELL);
    if(col < 0) col = 0;
    if(col >= GRID_COLS) col = GRID_COLS - 1;
    if(row < 0) row = 0;
    if(row >= GRID_ROWS) row = GRID_ROWS - 1;

    int bestCol = col;
    int bestRow = row;
    int baseDist = dist[col][row];
    int bestScore = (baseDist == -1) ? 1000000 : baseDist * 10;

    int dx[4] = {1, -1, 0, 0};
    int dy[4] = {0, 0, 1, -1};
    for(int d = 0; d < 4; d++){
        int nx = col + dx[d];
        int ny = row + dy[d];
        if(nx < 0 || nx >= GRID_COLS || ny < 0 || ny >= GRID_ROWS) continue;
        if(blocked[nx][ny]) continue;
        if(dist[nx][ny] == -1) continue;

        // scale distance up so it dominates, then let nearby-ghost penalties
        // only break ties / near-ties rather than overriding real progress
        int score = dist[nx][ny] * 10;

        for(int g = 0; g < ghostCount; g++){
            if(g == selfIndex || !allGhosts[g].active) continue;

            float gcx = (allGhosts[g].rect.x + allGhosts[g].rect.width / 2.0f) / GRID_CELL;
            float gcy = (allGhosts[g].rect.y + allGhosts[g].rect.height / 2.0f) / GRID_CELL;
            float cellDist = sqrtf((gcx - nx) * (gcx - nx) + (gcy - ny) * (gcy - ny));

            if(cellDist < GHOST_PATH_AVOID_RADIUS){
                score += GHOST_PATH_AVOID_PENALTY;
            }
        }

        if(score < bestScore){
            bestScore = score;
            bestCol = nx;
            bestRow = ny;
        }
    }

    // aim for the centre of that neighbouring cell and creep toward it
    float targetX = bestCol * GRID_CELL + GRID_CELL / 2.0f - ghost->width / 2.0f;
    float targetY = bestRow * GRID_CELL + GRID_CELL / 2.0f - ghost->height / 2.0f;

    if(ghost->x < targetX) ghost->x += speed;
    if(ghost->x > targetX) ghost->x -= speed;
    if(ghost->y < targetY) ghost->y += speed;
    if(ghost->y > targetY) ghost->y -= speed;
}

void separateGhosts(Ghost *ghosts, int ghostCount){
    for(int i = 0; i < ghostCount; i++){
        if(!ghosts[i].active) continue;

        for(int j = i + 1; j < ghostCount; j++){
            if(!ghosts[j].active) continue;

            float cxA = ghosts[i].rect.x + ghosts[i].rect.width / 2.0f;
            float cyA = ghosts[i].rect.y + ghosts[i].rect.height / 2.0f;
            float cxB = ghosts[j].rect.x + ghosts[j].rect.width / 2.0f;
            float cyB = ghosts[j].rect.y + ghosts[j].rect.height / 2.0f;

            float dx = cxB - cxA;
            float dy = cyB - cyA;
            float dist = sqrtf(dx * dx + dy * dy);

            if(dist > 0.0f && dist < GHOST_MIN_SEPARATION){
                float overlap = (GHOST_MIN_SEPARATION - dist) / 2.0f;
                float nx = dx / dist;
                float ny = dy / dist;
                ghosts[i].rect.x -= nx * overlap;
                ghosts[i].rect.y -= ny * overlap;
                ghosts[j].rect.x += nx * overlap;
                ghosts[j].rect.y += ny * overlap;
            } else if(dist == 0.0f){
                // exactly stacked (rare) - nudge one sideways so they don't stay glued together
                ghosts[j].rect.x += GHOST_MIN_SEPARATION / 2.0f;
            }
        }
    }
}

// ---- high score persistence ----
// a single plain-text number saved next to the executable. missing/corrupt
// file just reads back as 0 rather than crashing the game.
int loadHighScore(void){
    int hs = 0;
    FILE *f = fopen(HIGH_SCORE_FILE, "r");
    if(f){
        if(fscanf(f, "%d", &hs) != 1){
            hs = 0;
        }
        fclose(f);
    }
    return hs;
}

void saveHighScore(int hs){
    FILE *f = fopen(HIGH_SCORE_FILE, "w");
    if(f){
        fprintf(f, "%d", hs);
        fclose(f);
    }
}

// ---- resetting for a new run ----
void resetGame(Rectangle *player, float *rotation, bool *facingleft,
                bool *magicalwalls, bool *ghostmove, double *ghostModeStartTime,
                int *score, bool *mouthOpen, double *lastMouthSwap,
                Coin *coins, int coinCount,
                Ghost *ghosts, int ghostCount, int screenwidth){

    player->x = 0;
    player->y = 0;
    player->width = 40;
    player->height = 41;

    *rotation = 0.0f;
    *facingleft = false;
    *magicalwalls = false;
    *ghostmove = false;
    *ghostModeStartTime = 0.0;
    *score = 0;
    *mouthOpen = true;
    *lastMouthSwap = GetTime();

    for(int i = 0; i < coinCount; i++){
        coins[i].active = true;
        coins[i].respawnAt = 0.0;
    }

    for(int i = 0; i < ghostCount; i++){
        ghosts[i].rect.x = screenwidth;
        ghosts[i].rect.y = TUNNEL_ROW_Y;
        ghosts[i].active = false;
    }
}

int main(){
    const int screenwidth = 800;
    const int screenheight = 600;

    bool magicalwalls = false;

    bool ghostmove = false;

    float rotation = 0.0f;
    bool facingleft = false;

    // player
    Rectangle player = {0, 0, 40, 41};
    int speed = 3;

    // ---- score / game state ----
    GameState gameState = STATE_PLAYING;
    int score = 0;
    int highScore = loadHighScore();
    bool newHighScore = false;

    // ---- ghost roster ----
    Ghost ghosts[] = {
        { {screenwidth, TUNNEL_ROW_Y, 38, 41}, 0, false, 2.0 },   // red - enters through the tunnel 2s after ghost mode starts
        { {screenwidth, TUNNEL_ROW_Y, 38, 41}, 1, false, 5.0 },   // yellow - enters through the tunnel 5s after ghost mode starts
        // { {screenwidth, TUNNEL_ROW_Y, 38, 41}, 0, false, 10.0 },  <- example: a third ghost, 10s later
    };
    int ghostCount = sizeof(ghosts) / sizeof(ghosts[0]);

    // records the moment ghost mode first turns on, so each ghost's spawn
    // delay above counts seconds from that point rather than from GetTime()==0
    double ghostModeStartTime = 0.0;
    

    //magic walls lvl 4
    Rectangle magicalwall1 = {430, 0, 10, 45};    //41
    Rectangle magicalwall2 = {370, 0, 10, 45};

    // walls
    Rectangle walls[] = {

        //outer walls lvl 1
        {40, 45, 340, 10},
        {430, 45, 320, 10},
        {40, 45, 10, 250},
        {748, 45, 10, 250},
        {40, 345, 10, 210},
        {748, 345, 10, 210},
        {40, 545, 340, 10},
        {430, 545, 320, 10},
        {0, 285, 40, 10},
        {0, 345, 40, 10},
        {755, 285, 50, 10},
        {748, 345, 50, 10 },
        {370, 545, 10, 60},
        {430, 545, 10, 60},    // 14

        //inner walls lvl 2
        {220, 50, 10, 100},
        {590, 50, 10, 100},
        {220, 455, 10, 100},    // 17
        {590, 455, 10, 100},
        {100, 195, 223, 10},     // 19
        {485, 195, 220, 10},
        {100, 400, 223, 10},
        {485, 400, 220, 10},
        {220, 200, 10, 95},    // 23
        {590, 200, 10, 95},
        {220, 345, 10, 55},
        {590, 345, 10, 55},
        {370, 100, 10, 190},
        {430, 100, 10, 190},
        {370, 340, 10, 160},
        {430, 340, 10, 160},
        {100, 255, 10, 98},
        {310, 255, 10, 98},    // 32
        {485, 255, 10, 98},
        {690, 255, 10, 98},

        // box walls lvl 3
        {100, 100, 70, 50},  // 35
        {275, 100, 50, 50},
        {100, 455, 70, 45},
        {485, 100, 55, 50},
        {645, 100, 60, 50},
        {645, 455, 60, 47},
    };

    // counting how many walls we have inside arrey
    int wallcount = sizeof(walls) / sizeof(walls[0]);  

    // ---- build the ghost pathfinding grid ----
    static bool blocked[GRID_COLS][GRID_ROWS];
    static int ghostDist[GRID_COLS][GRID_ROWS];

    for(int cx = 0; cx < GRID_COLS; cx++){
        for(int cy = 0; cy < GRID_ROWS; cy++){
            Rectangle cellRect = { cx * GRID_CELL, cy * GRID_CELL, GRID_CELL, GRID_CELL };

            blocked[cx][cy] = false;
            for(int i = 0; i < wallcount; i++){
                if(CheckCollisionRecs(cellRect, walls[i])){
                    blocked[cx][cy] = true;
                    break;
                }
            }
            if(CheckCollisionRecs(cellRect, magicalwall1) || CheckCollisionRecs(cellRect, magicalwall2)){
                blocked[cx][cy] = true;
            }
        }
    }

    // ---- build the coin grid ----
    Coin coins[MAX_COINS];
    int coinCount = 0;

    for(int gx = 60; gx <= 720 && coinCount < MAX_COINS; gx += CELL_SIZE){
        for(int gy = 60; gy <= 520 && coinCount < MAX_COINS; gy += CELL_SIZE){

            Rectangle candidate = { gx - COIN_SIZE/2.0f, gy - COIN_SIZE/2.0f, COIN_SIZE, COIN_SIZE };

            bool blocked = false;
            for(int i = 0; i < wallcount; i++){
                if(CheckCollisionRecs(candidate, walls[i])){
                    blocked = true;
                    break;
                }
            }

            if(!blocked){
                coins[coinCount].rect = candidate;
                coins[coinCount].active = true;
                coins[coinCount].respawnAt = 0.0;
                coinCount++;
            }
        }
    }
        
    // load assets
    Image playerimage = LoadImage("assets/pacmano.png");
    Image playerimage2 = LoadImage("assets/pacmanc.png");
    Image ghost1image = LoadImage("assets/ghost1.png");
    Image ghost2image = LoadImage("assets/ghost2.png");

    

    InitWindow(screenwidth, screenheight, "Pac Man");
    SetTargetFPS(60);
    srand(time(NULL));

    // convert image into texture for GPU
    Texture2D playertexture = LoadTextureFromImage(playerimage);
    Texture2D playertexture1 = LoadTextureFromImage(playerimage2);
    Texture2D ghost1texture = LoadTextureFromImage(ghost1image);
    Texture2D ghost2texture = LoadTextureFromImage(ghost2image);

    // ghosts pick their texture by index into this array (see textureIndex
    Texture2D ghostTextures[] = { ghost1texture, ghost2texture };
    int ghostTextureCount = sizeof(ghostTextures) / sizeof(ghostTextures[0]);

    // load audio
    InitAudioDevice();
    Sound coinSound = LoadSound("assets/coin.mp3");
    Sound gameOverSound = LoadSound("assets/defeat.mp3");
    Sound newHighScoreSound = LoadSound("assets/newhighscore.mp3");

    // pool of aliases sharing the same coin sound data. PlaySound() on a
    #define COIN_SOUND_POOL 16
    Sound coinSoundPool[COIN_SOUND_POOL];
    for(int i = 0; i < COIN_SOUND_POOL; i++){
        coinSoundPool[i] = LoadSoundAlias(coinSound);
    }
    int coinSoundIndex = 0;

    // ---- mouth chewing animation ----
    // swap between the open-mouth and closed-mouth sprites while pacman is
    // moving, like the classic chomping look. frozen open when standing still.
    #define MOUTH_ANIM_INTERVAL 0.08 // seconds between each open/closed swap
    bool mouthOpen = true;
    double lastMouthSwap = 0.0;

    while(!WindowShouldClose()){

        if(gameState == STATE_PLAYING){

            float oldx = player.x;
            float oldy = player.y;

            // used below to animate the chewing mouth - only chomp while actually moving
            bool moving = IsKeyDown(KEY_A) || IsKeyDown(KEY_D) || IsKeyDown(KEY_W) || IsKeyDown(KEY_S);

            // movement horizontal
            if(IsKeyDown(KEY_A)){
                player.x = player.x - speed;
                facingleft = true;
                rotation = 0.0f;
            }
            if(IsKeyDown(KEY_D)){
                player.x = player.x + speed;
                facingleft = false;
                rotation = 0.0f;
            }
            // horizontal collision
            // use a slightly smaller box than the sprite so doorways sized to
            // fit the player don't reject you over a few pixels of misalignment
            Rectangle playerCollider = {
                player.x + PLAYER_COLLISION_MARGIN,
                player.y + PLAYER_COLLISION_MARGIN,
                player.width - PLAYER_COLLISION_MARGIN * 2,
                player.height - PLAYER_COLLISION_MARGIN * 2
            };
            for(int i=0;i < wallcount;i++){
                // collision for player and walls
                if(CheckCollisionRecs(playerCollider, walls[i])){
                    player.x = oldx;
                    break;
                }
            }

            // movement vertical
            if(IsKeyDown(KEY_W)){
                player.y = player.y - speed;
                rotation = 270.0f;
                facingleft = false;
            }
            if(IsKeyDown(KEY_S)){
                player.y = player.y + speed;
                rotation = 90.0f;
                facingleft = false;
            }
            // vertical collision - recompute the collider since player.y moved
            playerCollider.x = player.x + PLAYER_COLLISION_MARGIN;
            playerCollider.y = player.y + PLAYER_COLLISION_MARGIN;
            for(int i=0;i < wallcount;i++){
                // collision for player and walls
                if(CheckCollisionRecs(playerCollider, walls[i])){
                    player.y = oldy;
                    break;
                }
            }    

            // collision for window
            if(magicalwalls == false){
                if(player.y <= 0){
                    player.y = 0;
                }
                if(player.y > screenheight - player.height){
                    player.y = screenheight - player.height;
                }
                if(player.x <= 0){
                    player.x = 0;
                }
                if(player.x > screenwidth - player.width){
                    player.x = screenwidth - player.width;
                }
            }
            
            // magical walls logic
            if(player.x >= 385 && player.y > 35){
                if(ghostmove == false){
                    ghostModeStartTime = GetTime(); // starting line for every ghost's spawnDelay countdown
                }
                magicalwalls = true;
                ghostmove = true;
            }

            if(magicalwalls == true){
                playerCollider.x = player.x + PLAYER_COLLISION_MARGIN;
                playerCollider.y = player.y + PLAYER_COLLISION_MARGIN;
                if(CheckCollisionRecs(playerCollider, magicalwall1)){
                    player.x = oldx;
                }
                if(CheckCollisionRecs(playerCollider, magicalwall2)){
                    player.x = oldx;
                }
            }


            // screen wrapping
            // when the player fully exits one side, snap them to just outside the
            // opposite side (not just "subtract width once") so the teleport is
            // always clean no matter how fast they were moving when it triggered.
            if(magicalwalls == true){
                if(player.x + player.width < 0){
                    player.x = screenwidth;
                }
                if(player.x > screenwidth){
                    player.x = -player.width;
                }
                if(player.y + player.height < 0){
                    player.y = screenheight;
                }
                if(player.y > screenheight){
                    player.y = -player.height;
                }
            }

            // ghost movement
            // find the player's grid cell, flood-fill the distance-to-player
            // across every open cell once per frame, then let each active ghost
            // step into whichever neighbouring cell brings it closer - this
            // makes them flow through corridors instead of walking into walls.
            if(ghostmove == true){
                double elapsed = GetTime() - ghostModeStartTime;

                // ---- spawn-in timer ----
                // any ghost still waiting whose delay has passed enters the map now
                for(int i = 0; i < ghostCount; i++){
                    if(!ghosts[i].active && elapsed >= ghosts[i].spawnDelay){
                        ghosts[i].active = true;
                    }
                }

                int playerCol = (int)((player.x + player.width / 2) / GRID_CELL);
                int playerRow = (int)((player.y + player.height / 2) / GRID_CELL);
                if(playerCol < 0) playerCol = 0;
                if(playerCol >= GRID_COLS) playerCol = GRID_COLS - 1;
                if(playerRow < 0) playerRow = 0;
                if(playerRow >= GRID_ROWS) playerRow = GRID_ROWS - 1;

                computeDistanceField(blocked, playerCol, playerRow, ghostDist);

                for(int i = 0; i < ghostCount; i++){
                    if(!ghosts[i].active) continue; // still waiting to spawn in, skip it

                    moveGhostAlongPath(&ghosts[i].rect, ghostDist, blocked, 2.0f, ghosts, ghostCount, i);

                    // same tunnel wrap-around the player gets: walk off one
                    // edge, reappear on the other. this is also how a delayed
                    // ghost, starting parked just past the edge, walks onto
                    // the visible map in the first place.
                    if(ghosts[i].rect.x + ghosts[i].rect.width < 0) ghosts[i].rect.x = screenwidth;
                    if(ghosts[i].rect.x > screenwidth) ghosts[i].rect.x = -ghosts[i].rect.width;
                    if(ghosts[i].rect.y + ghosts[i].rect.height < 0) ghosts[i].rect.y = screenheight;
                    if(ghosts[i].rect.y > screenheight) ghosts[i].rect.y = -ghosts[i].rect.height;
                }

                // hard push-apart pass so ghosts never end up drawn on top of
                // each other, even if the path-avoidance steering above still
                // sent two of them into neighbouring cells
                separateGhosts(ghosts, ghostCount);

                // ---- catch check ----
                // any active ghost touching the player ends the run
                Rectangle playerHitbox = {
                    player.x + PLAYER_COLLISION_MARGIN,
                    player.y + PLAYER_COLLISION_MARGIN,
                    player.width - PLAYER_COLLISION_MARGIN * 2,
                    player.height - PLAYER_COLLISION_MARGIN * 2
                };
                for(int i = 0; i < ghostCount; i++){
                    if(!ghosts[i].active) continue;
                    if(CheckCollisionRecs(playerHitbox, ghosts[i].rect)){
                        PlaySound(gameOverSound);
                        gameState = STATE_GAME_OVER;
                        newHighScore = false;
                        if(score > highScore){
                            highScore = score;
                            saveHighScore(highScore);
                            PlaySound(newHighScoreSound);
                            newHighScore = true;
                        }
                        break;
                    }
                }
            }

            // ---- coin eating logic ----
            // check every active coin against the player. if we touch one,
            // turn it off, remember when it should come back, award points,
            // and play the sound.
            for(int i = 0; i < coinCount; i++){
                if(coins[i].active && CheckCollisionRecs(player, coins[i].rect)){
                    coins[i].active = false;
                    coins[i].respawnAt = GetTime() + COIN_RESPAWN_TIME;
                    score += COIN_SCORE_VALUE;

                    // play the next alias in the pool, then move on to the one
                    // after it for next time. this is what lets every single
                    // coin play its sound, even if you eat several in a row.
                    PlaySound(coinSoundPool[coinSoundIndex]);
                    coinSoundIndex = (coinSoundIndex + 1) % COIN_SOUND_POOL;
                }
            }

            // ---- coin respawn logic ----
            // any coin that has been gone long enough simply turns back on.
            for(int i = 0; i < coinCount; i++){
                if(!coins[i].active && GetTime() >= coins[i].respawnAt){
                    coins[i].active = true;
                }
            }


            // ---- mouth chewing animation ----
            // flip between open/closed every MOUTH_ANIM_INTERVAL seconds while
            // moving. standing still just holds the mouth open, like the original.
            if(moving){
                if(GetTime() - lastMouthSwap >= MOUTH_ANIM_INTERVAL){
                    mouthOpen = !mouthOpen;
                    lastMouthSwap = GetTime();
                }
            } else {
                mouthOpen = true;
            }

        } else { // STATE_GAME_OVER - wait for the player's replay choice
            if(IsKeyPressed(KEY_Y)){
                resetGame(&player, &rotation, &facingleft,
                          &magicalwalls, &ghostmove, &ghostModeStartTime,
                          &score, &mouthOpen, &lastMouthSwap,
                          coins, coinCount,
                          ghosts, ghostCount, screenwidth);
                gameState = STATE_PLAYING;
            }
            if(IsKeyPressed(KEY_N) || IsKeyPressed(KEY_ESCAPE)){
                PlaySound(gameOverSound);
                break; // fall out of the main loop and clean up below
            }
        }


        BeginDrawing();
        ClearBackground(BLACK);

        // drawing pacman
        // pick whichever sprite the chewing animation says to use this frame
        Texture2D currentPlayerTexture = mouthOpen ? playertexture : playertexture1;
        float sourceWidth = currentPlayerTexture.width;

        if(facingleft){
            sourceWidth = -currentPlayerTexture.width;
        }


        DrawTexturePro(currentPlayerTexture,
            (Rectangle){0, 0,
            sourceWidth,
            currentPlayerTexture.height
            },
            (Rectangle){
                player.x + player.width / 2,
                player.y + player.height / 2,
                player.width,
                player.height
            },
            (Vector2){
                player.width / 2,
                player.height / 2
            },
            rotation,
            WHITE
        );
        // drawing ghosts
        // only draw ones that have actually spawned in - a ghost still
        // waiting on its timer stays invisible until it enters
        for(int i = 0; i < ghostCount; i++){
            if(!ghosts[i].active) continue;

            Texture2D tex = ghostTextures[ghosts[i].textureIndex % ghostTextureCount];
            DrawTexturePro(tex,
                (Rectangle){0, 0, (float)tex.width, (float)tex.height},
                (Rectangle){ghosts[i].rect.x, ghosts[i].rect.y, 40, 41},
                (Vector2){0, 0},
                0.0f,
                WHITE);
        }

        //drawing magical walls
        if(magicalwalls == true){
            DrawRectangleRec(magicalwall1, BLUE);
            DrawRectangleRec(magicalwall2, BLUE);
        }

        // drawing walls
        for(int i=0; i < wallcount;i++){
            DrawRectangleRec(walls[i], BLUE);
        }

        // drawing coins
        // every coin is the same size/color so the grid reads as uniform dots
        for(int i = 0; i < coinCount; i++){
            if(coins[i].active){
                float cx = coins[i].rect.x + COIN_SIZE / 2.0f;
                float cy = coins[i].rect.y + COIN_SIZE / 2.0f;
                DrawCircle((int)cx, (int)cy, COIN_SIZE / 2.0f, YELLOW);
            }
        }

        // ---- scoreboard ----
        // sits in the empty strip above the maze (walls start at y=45)
        DrawText(TextFormat("SCORE: %d", score), 10, 8, 20, WHITE);
        DrawText(TextFormat("HIGH SCORE: %d", highScore), 10, 30, 20, YELLOW);

        // ---- game over overlay ----
        if(gameState == STATE_GAME_OVER){
            DrawRectangle(0, 0, screenwidth, screenheight, (Color){0, 0, 0, 180});

            const char *title = "GAME OVER";
            DrawText(title, screenwidth / 2 - MeasureText(title, 50) / 2, screenheight / 2 - 100, 50, RED);

            const char *scoreLine = TextFormat("Score: %d    High Score: %d", score, highScore);
            DrawText(scoreLine, screenwidth / 2 - MeasureText(scoreLine, 24) / 2, screenheight / 2 - 30, 24, WHITE);

            if(newHighScore){
                const char *newHigh = "NEW HIGH SCORE!";
                DrawText(newHigh, screenwidth / 2 - MeasureText(newHigh, 22) / 2, screenheight / 2 + 4, 22, YELLOW);
                PlaySound(newHighScoreSound);
            }

            const char *prompt = "Play again? Press Y for yes, N to quit";
            DrawText(prompt, screenwidth / 2 - MeasureText(prompt, 22) / 2, screenheight / 2 + 46, 22, LIGHTGRAY);
        }

        DrawText("Developed by: SAILESH DAHAL  ", 15, 565, 15, GREEN);
        EndDrawing();
    }

    // unloading texture
    UnloadTexture(playertexture);
    UnloadTexture(playertexture1);
    UnloadTexture(ghost1texture);
    UnloadTexture(ghost2texture);
    UnloadImage(playerimage);
    UnloadImage(playerimage2);
    UnloadImage(ghost1image);
    UnloadImage(ghost2image);
    for(int i = 0; i < COIN_SOUND_POOL; i++){
        UnloadSoundAlias(coinSoundPool[i]);
    }
    UnloadSound(coinSound);
    UnloadSound(gameOverSound);
    UnloadSound(newHighScoreSound);
    CloseAudioDevice();

    CloseWindow();
    return 0;
}