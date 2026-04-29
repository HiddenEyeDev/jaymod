#ifndef GAME_CMD_SETPROFILE_H
#define GAME_CMD_SETPROFILE_H

///////////////////////////////////////////////////////////////////////////////

class SetProfile : public AbstractBuiltin
{
protected:
    PostAction doExecute( Context& );

public:
    SetProfile();
    ~SetProfile();
};

///////////////////////////////////////////////////////////////////////////////

#endif // GAME_CMD_SETPROFILE_H
