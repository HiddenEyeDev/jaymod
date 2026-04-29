#ifndef GAME_CMD_ACHIEVEMENTS_H
#define GAME_CMD_ACHIEVEMENTS_H

///////////////////////////////////////////////////////////////////////////////

class Achievements : public AbstractBuiltin
{
protected:
    PostAction doExecute( Context& );

public:
    Achievements();
    ~Achievements();
};

///////////////////////////////////////////////////////////////////////////////

#endif // GAME_CMD_ACHIEVEMENTS_H
