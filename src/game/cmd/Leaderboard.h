#ifndef GAME_CMD_LEADERBOARD_H
#define GAME_CMD_LEADERBOARD_H

///////////////////////////////////////////////////////////////////////////////

class Leaderboard : public AbstractBuiltin
{
protected:
    PostAction doExecute( Context& );

public:
    Leaderboard();
    ~Leaderboard();
};

///////////////////////////////////////////////////////////////////////////////

#endif // GAME_CMD_LEADERBOARD_H
