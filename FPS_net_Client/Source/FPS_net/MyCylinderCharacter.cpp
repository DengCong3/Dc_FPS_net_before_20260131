// Fill out your copyright notice in the Description page of Project Settings.

#include "MyCylinderCharacter.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"

AMyCylinderCharacter::AMyCylinderCharacter()
{
    PrimaryActorTick.bCanEverTick = true;

    // 初始化指针（可选，但推荐）
    DefaultMappingContext = nullptr;
    MoveAction = nullptr;
    LookAction = nullptr;
}

void AMyCylinderCharacter::BeginPlay()
{
    Super::BeginPlay();

    // 应用默认输入映射上下文
    if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
        {
            Subsystem->AddMappingContext(DefaultMappingContext, 0);
        }
    }
}

void AMyCylinderCharacter::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    // 如果需要每帧逻辑，可在此添加
}

void AMyCylinderCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    if (UEnhancedInputComponent* EnhancedInputComponent = CastChecked<UEnhancedInputComponent>(PlayerInputComponent))
    {
        // 绑定移动
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AMyCylinderCharacter::Move);

        // 绑定视角
        EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AMyCylinderCharacter::Look);
    }
}

void AMyCylinderCharacter::Move(const FInputActionValue& Value)
{
    FVector2D MovementVector = Value.Get<FVector2D>();

    if (Controller && MovementVector.SizeSquared() > 0.0f)
    {
        FRotator Rotation = Controller->GetControlRotation();
        FRotator YawRotation(0, Rotation.Yaw, 0);

        FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
        AddMovementInput(ForwardDirection, MovementVector.X);

        FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
        AddMovementInput(RightDirection, MovementVector.Y);
    }
}

void AMyCylinderCharacter::Look(const FInputActionValue& Value)
{
    FVector2D LookAxisVector = Value.Get<FVector2D>();

    if (Controller && LookAxisVector.SizeSquared() > 0.0f)
    {
        AddControllerYawInput(LookAxisVector.X);
        AddControllerPitchInput(-LookAxisVector.Y); // 负号确保鼠标下移视角向下
    }
}